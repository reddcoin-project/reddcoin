// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/reddid_db.h>
#include <test/util/setup_common.h>
#include <key.h>
#include <uint256.h>
#include <amount.h>
#include <clientversion.h>
#include <dbwrapper.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <util/strencodings.h>

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct ReddIDDBTestingSetup : public BasicTestingSetup {
    std::unique_ptr<ReddIDDB> db;
    ReddIDDBTestingSetup()
    {
        db = std::make_unique<ReddIDDB>(1 << 20, true, false);
    }
    ~ReddIDDBTestingSetup() { db.reset(); }
};

BOOST_FIXTURE_TEST_SUITE(reddid_db_tests, ReddIDDBTestingSetup)

// ===================================================================
// 1. Namespace DB
// ===================================================================

BOOST_AUTO_TEST_CASE(namespace_write_read_roundtrip)
{
    NamespaceInfo nsIn;
    nsIn.id = "redd";
    nsIn.owner = CKeyID(uint160(std::vector<unsigned char>(20, 0xAA)));
    nsIn.allowNumbers = true;
    nsIn.allowHyphens = true;
    nsIn.allowUnderscores = false;
    nsIn.minLength = 3;
    nsIn.maxLength = 24;
    nsIn.renewalPeriod = 365;
    nsIn.gracePeriod = 30;
    nsIn.namespaceRevenuePct = 10;
    nsIn.burnPct = 70;
    nsIn.nodePct = 15;
    nsIn.devPct = 5;
    nsIn.minAuctionDuration = 3;
    nsIn.maxAuctionDuration = 7;
    nsIn.minBidIncrement = 7.55;
    nsIn.configHash = uint256S("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    nsIn.lastUpdated = 1700000000;
    nsIn.expiration = 1800000000;

    BOOST_CHECK(db->WriteNamespace("redd", nsIn));

    NamespaceInfo nsOut;
    BOOST_CHECK(db->ReadNamespace("redd", nsOut));
    BOOST_CHECK_EQUAL(nsOut.id, "redd");
    BOOST_CHECK(nsOut.owner == nsIn.owner);
    BOOST_CHECK_EQUAL(nsOut.allowNumbers, true);
    BOOST_CHECK_EQUAL(nsOut.minLength, 3);
    BOOST_CHECK_EQUAL(nsOut.maxLength, 24);
    BOOST_CHECK_EQUAL(nsOut.namespaceRevenuePct, 10);
    BOOST_CHECK_EQUAL(nsOut.burnPct, 70);
    // minBidIncrement: 7.55 -> int32(755) -> 7.55
    BOOST_CHECK(std::fabs(nsOut.minBidIncrement - 7.55) < 0.01);
    BOOST_CHECK(nsOut.configHash == nsIn.configHash);
    BOOST_CHECK_EQUAL(nsOut.lastUpdated, 1700000000);
    BOOST_CHECK_EQUAL(nsOut.expiration, 1800000000);
}

BOOST_AUTO_TEST_CASE(namespace_exists_after_write_and_erase)
{
    NamespaceInfo ns;
    ns.id = "crypto";
    BOOST_CHECK(!db->ExistsNamespace("crypto"));
    BOOST_CHECK(db->WriteNamespace("crypto", ns));
    BOOST_CHECK(db->ExistsNamespace("crypto"));
    BOOST_CHECK(db->EraseNamespace("crypto"));
    BOOST_CHECK(!db->ExistsNamespace("crypto"));
}

BOOST_AUTO_TEST_CASE(namespace_list_all)
{
    NamespaceInfo ns1; ns1.id = "alpha";
    NamespaceInfo ns2; ns2.id = "beta";
    NamespaceInfo ns3; ns3.id = "gamma";
    BOOST_CHECK(db->WriteNamespace("alpha", ns1));
    BOOST_CHECK(db->WriteNamespace("beta", ns2));
    BOOST_CHECK(db->WriteNamespace("gamma", ns3));

    std::vector<std::string> ids;
    BOOST_CHECK(db->ListNamespaces(ids));
    BOOST_CHECK_EQUAL(ids.size(), 3u);
    std::set<std::string> idSet(ids.begin(), ids.end());
    BOOST_CHECK(idSet.count("alpha") == 1);
    BOOST_CHECK(idSet.count("beta") == 1);
    BOOST_CHECK(idSet.count("gamma") == 1);
}

BOOST_AUTO_TEST_CASE(namespace_erase_removes_entry)
{
    NamespaceInfo ns; ns.id = "temp";
    BOOST_CHECK(db->WriteNamespace("temp", ns));
    BOOST_CHECK(db->EraseNamespace("temp"));
    NamespaceInfo nsOut;
    BOOST_CHECK(!db->ReadNamespace("temp", nsOut));
}

BOOST_AUTO_TEST_CASE(namespace_overwrite_existing)
{
    NamespaceInfo ns; ns.id = "social"; ns.minLength = 5;
    BOOST_CHECK(db->WriteNamespace("social", ns));
    NamespaceInfo nsUpd; nsUpd.id = "social"; nsUpd.minLength = 4;
    BOOST_CHECK(db->WriteNamespace("social", nsUpd));
    NamespaceInfo nsOut;
    BOOST_CHECK(db->ReadNamespace("social", nsOut));
    BOOST_CHECK_EQUAL(nsOut.minLength, 4);
}

BOOST_AUTO_TEST_CASE(namespace_read_nonexistent)
{
    NamespaceInfo nsOut;
    BOOST_CHECK(!db->ReadNamespace("nonexistent", nsOut));
}

// ===================================================================
// 2. PricingTier DB
// ===================================================================

BOOST_AUTO_TEST_CASE(pricingtier_write_read_multiple)
{
    PricingTier t1; t1.namespaceId = "redd"; t1.minLength = 3; t1.minPrice = 50000 * COIN;
    PricingTier t2; t2.namespaceId = "redd"; t2.minLength = 5; t2.minPrice = 10000 * COIN;
    PricingTier t3; t3.namespaceId = "redd"; t3.minLength = 8; t3.minPrice = 1000 * COIN;
    BOOST_CHECK(db->WritePricingTier(t1));
    BOOST_CHECK(db->WritePricingTier(t2));
    BOOST_CHECK(db->WritePricingTier(t3));

    std::vector<PricingTier> tiers;
    BOOST_CHECK(db->ReadPricingTiers("redd", tiers));
    BOOST_CHECK_EQUAL(tiers.size(), 3u);
}

BOOST_AUTO_TEST_CASE(pricingtier_erase_specific)
{
    PricingTier t1; t1.namespaceId = "erase"; t1.minLength = 3; t1.minPrice = 100000 * COIN;
    PricingTier t2; t2.namespaceId = "erase"; t2.minLength = 6; t2.minPrice = 5000 * COIN;
    BOOST_CHECK(db->WritePricingTier(t1));
    BOOST_CHECK(db->WritePricingTier(t2));
    BOOST_CHECK(db->ErasePricingTier("erase", 3));
    std::vector<PricingTier> tiers;
    BOOST_CHECK(db->ReadPricingTiers("erase", tiers));
    BOOST_CHECK_EQUAL(tiers.size(), 1u);
    BOOST_CHECK_EQUAL(tiers[0].minLength, 6);
}

BOOST_AUTO_TEST_CASE(pricingtier_read_nonexistent)
{
    std::vector<PricingTier> tiers;
    BOOST_CHECK(db->ReadPricingTiers("nonexistent", tiers));
    BOOST_CHECK(tiers.empty());
}

// ===================================================================
// 3. UserID DB
// ===================================================================

BOOST_AUTO_TEST_CASE(userid_write_read_roundtrip)
{
    UserIDInfo uidIn;
    uidIn.name = "alice";
    uidIn.namespaceId = "redd";
    uidIn.owner = CKeyID(uint160(std::vector<unsigned char>(20, 0xBB)));
    uidIn.registrationTime = 1700000000;
    uidIn.expirationTime = 1750000000;
    uidIn.lastTransaction = 1700500000;
    uidIn.transactionCount = 42;
    uidIn.metadataHash = uint256S("1111111111111111111111111111111111111111111111111111111111111111");

    BOOST_CHECK(db->WriteUserID("alice", "redd", uidIn));
    UserIDInfo uidOut;
    BOOST_CHECK(db->ReadUserID("alice", "redd", uidOut));
    BOOST_CHECK_EQUAL(uidOut.name, "alice");
    BOOST_CHECK_EQUAL(uidOut.namespaceId, "redd");
    BOOST_CHECK(uidOut.owner == uidIn.owner);
    BOOST_CHECK_EQUAL(uidOut.registrationTime, 1700000000);
    BOOST_CHECK_EQUAL(uidOut.transactionCount, 42);
}

BOOST_AUTO_TEST_CASE(userid_exists_true_false)
{
    BOOST_CHECK(!db->ExistsUserID("bob", "redd"));
    UserIDInfo uid; uid.name = "bob"; uid.namespaceId = "redd";
    BOOST_CHECK(db->WriteUserID("bob", "redd", uid));
    BOOST_CHECK(db->ExistsUserID("bob", "redd"));
    BOOST_CHECK(!db->ExistsUserID("bob", "crypto"));
}

BOOST_AUTO_TEST_CASE(userid_list_for_namespace)
{
    UserIDInfo u1; u1.name = "alice"; u1.namespaceId = "social";
    UserIDInfo u2; u2.name = "bob"; u2.namespaceId = "social";
    UserIDInfo u3; u3.name = "carol"; u3.namespaceId = "other";
    BOOST_CHECK(db->WriteUserID("alice", "social", u1));
    BOOST_CHECK(db->WriteUserID("bob", "social", u2));
    BOOST_CHECK(db->WriteUserID("carol", "other", u3));

    std::vector<std::string> names;
    BOOST_CHECK(db->ListUserIDs("social", names));
    BOOST_CHECK_EQUAL(names.size(), 2u);
    std::set<std::string> nameSet(names.begin(), names.end());
    BOOST_CHECK(nameSet.count("alice") == 1);
    BOOST_CHECK(nameSet.count("bob") == 1);
}

BOOST_AUTO_TEST_CASE(userid_erase_removes_entry)
{
    UserIDInfo uid; uid.name = "dave"; uid.namespaceId = "redd";
    BOOST_CHECK(db->WriteUserID("dave", "redd", uid));
    BOOST_CHECK(db->EraseUserID("dave", "redd"));
    BOOST_CHECK(!db->ExistsUserID("dave", "redd"));
}

// ===================================================================
// 4. Auction DB
// ===================================================================

BOOST_AUTO_TEST_CASE(auction_write_read_roundtrip)
{
    uint256 auctionId = uint256S("aaaa000000000000000000000000000000000000000000000000000000000001");
    AuctionInfo aIn;
    aIn.auctionId = auctionId;
    aIn.name = "premium";
    aIn.namespaceId = "redd";
    aIn.creator = CKeyID(uint160(std::vector<unsigned char>(20, 0xCC)));
    aIn.startTime = 1700000000;
    aIn.endTime = 1700100000;
    aIn.reservePrice = 5000 * COIN;
    aIn.currentBid = 7500 * COIN;
    aIn.depositAmount = 500 * COIN;
    aIn.state = AUCTION_ACTIVE;
    aIn.type = AUCTION_PREMIUM;
    aIn.blockHeight = 100000;
    aIn.txid = uint256S("3333333333333333333333333333333333333333333333333333333333333333");

    BOOST_CHECK(db->WriteAuction(auctionId, aIn));
    AuctionInfo aOut;
    BOOST_CHECK(db->ReadAuction(auctionId, aOut));
    BOOST_CHECK(aOut.auctionId == auctionId);
    BOOST_CHECK_EQUAL(aOut.name, "premium");
    BOOST_CHECK(aOut.state == AUCTION_ACTIVE);
    BOOST_CHECK(aOut.type == AUCTION_PREMIUM);
    BOOST_CHECK_EQUAL(aOut.blockHeight, 100000);
}

BOOST_AUTO_TEST_CASE(auction_list_all)
{
    uint256 id1 = uint256S("bbbb000000000000000000000000000000000000000000000000000000000001");
    uint256 id2 = uint256S("bbbb000000000000000000000000000000000000000000000000000000000002");
    AuctionInfo a1; a1.auctionId = id1; a1.state = AUCTION_PENDING; a1.type = AUCTION_STANDARD;
    AuctionInfo a2; a2.auctionId = id2; a2.state = AUCTION_ACTIVE; a2.type = AUCTION_STANDARD;
    BOOST_CHECK(db->WriteAuction(id1, a1));
    BOOST_CHECK(db->WriteAuction(id2, a2));

    std::vector<uint256> ids;
    BOOST_CHECK(db->ListAuctions(ids));
    BOOST_CHECK_EQUAL(ids.size(), 2u);
}

BOOST_AUTO_TEST_CASE(auction_list_by_state)
{
    uint256 idP = uint256S("cccc000000000000000000000000000000000000000000000000000000000001");
    uint256 idA = uint256S("cccc000000000000000000000000000000000000000000000000000000000002");
    AuctionInfo aP; aP.auctionId = idP; aP.state = AUCTION_PENDING; aP.type = AUCTION_STANDARD;
    AuctionInfo aA; aA.auctionId = idA; aA.state = AUCTION_ACTIVE; aA.type = AUCTION_STANDARD;
    BOOST_CHECK(db->WriteAuction(idP, aP));
    BOOST_CHECK(db->WriteAuction(idA, aA));

    std::vector<uint256> activeIds;
    BOOST_CHECK(db->ListAuctionsByState(AUCTION_ACTIVE, activeIds));
    BOOST_CHECK_EQUAL(activeIds.size(), 1u);
    BOOST_CHECK(activeIds[0] == idA);
}

BOOST_AUTO_TEST_CASE(auction_erase_removes_entry)
{
    uint256 auctionId = uint256S("dddd000000000000000000000000000000000000000000000000000000000001");
    AuctionInfo a; a.auctionId = auctionId; a.state = AUCTION_PENDING; a.type = AUCTION_STANDARD;
    BOOST_CHECK(db->WriteAuction(auctionId, a));
    BOOST_CHECK(db->EraseAuction(auctionId));
    BOOST_CHECK(!db->ExistsAuction(auctionId));
}

// ===================================================================
// 5. Bid DB
// ===================================================================

BOOST_AUTO_TEST_CASE(bid_write_read_roundtrip)
{
    uint256 bidId = uint256S("eeee000000000000000000000000000000000000000000000000000000000001");
    uint256 auctionId = uint256S("ffff000000000000000000000000000000000000000000000000000000000001");
    BidInfo bIn;
    bIn.bidId = bidId;
    bIn.auctionId = auctionId;
    bIn.bidder = CKeyID(uint160(std::vector<unsigned char>(20, 0xEE)));
    bIn.bidderReddId = "alice.redd";
    bIn.bidAmount = 10000 * COIN;
    bIn.depositAmount = 1000 * COIN;
    bIn.bidTime = 1700050000;
    bIn.isWinner = false;
    bIn.refunded = false;

    BOOST_CHECK(db->WriteBid(bidId, bIn));
    BidInfo bOut;
    BOOST_CHECK(db->ReadBid(bidId, bOut));
    BOOST_CHECK(bOut.bidId == bidId);
    BOOST_CHECK(bOut.auctionId == auctionId);
    BOOST_CHECK_EQUAL(bOut.bidderReddId, "alice.redd");
    BOOST_CHECK_EQUAL(bOut.bidAmount, 10000 * COIN);
    BOOST_CHECK_EQUAL(bOut.isWinner, false);
    BOOST_CHECK_EQUAL(bOut.refunded, false);
}

BOOST_AUTO_TEST_CASE(bid_list_by_auction)
{
    uint256 aId = uint256S("a0a0000000000000000000000000000000000000000000000000000000000001");
    uint256 bId1 = uint256S("b1b1000000000000000000000000000000000000000000000000000000000001");
    uint256 bId2 = uint256S("b1b1000000000000000000000000000000000000000000000000000000000002");
    BidInfo b1; b1.bidId = bId1; b1.auctionId = aId; b1.bidAmount = 5000 * COIN;
    BidInfo b2; b2.bidId = bId2; b2.auctionId = aId; b2.bidAmount = 7000 * COIN;
    BOOST_CHECK(db->WriteBid(bId1, b1));
    BOOST_CHECK(db->WriteBid(bId2, b2));

    std::vector<uint256> bids;
    BOOST_CHECK(db->ListBids(aId, bids));
    BOOST_CHECK_EQUAL(bids.size(), 2u);
}

BOOST_AUTO_TEST_CASE(bid_update_status)
{
    uint256 bidId = uint256S("c2c2000000000000000000000000000000000000000000000000000000000001");
    BidInfo bid; bid.bidId = bidId; bid.bidAmount = 8000 * COIN; bid.isWinner = false; bid.refunded = false;
    BOOST_CHECK(db->WriteBid(bidId, bid));

    BOOST_CHECK(db->UpdateBidStatus(bidId, true, false));
    BidInfo bOut;
    BOOST_CHECK(db->ReadBid(bidId, bOut));
    BOOST_CHECK_EQUAL(bOut.isWinner, true);
    BOOST_CHECK_EQUAL(bOut.refunded, false);

    BOOST_CHECK(db->UpdateBidStatus(bidId, true, true));
    BidInfo bOut2;
    BOOST_CHECK(db->ReadBid(bidId, bOut2));
    BOOST_CHECK_EQUAL(bOut2.refunded, true);
}

// ===================================================================
// 6. Profile DB
// ===================================================================

BOOST_AUTO_TEST_CASE(profile_write_read_roundtrip)
{
    ReddIDProfile pIn;
    pIn.reddId = "alice";
    pIn.owner = CKeyID(uint160(std::vector<unsigned char>(20, 0xFF)));
    pIn.displayName = "Alice Wonderland";
    pIn.bio = "Blockchain enthusiast";
    pIn.verificationStatus = VERIFICATION_COMMUNITY;
    pIn.creationTime = 1700000000;
    pIn.lastUpdated = 1700500000;
    pIn.expirationTime = 1800000000;
    pIn.active = true;
    pIn.flags = 0x03;

    BOOST_CHECK(db->WriteProfile("alice", pIn));
    ReddIDProfile pOut;
    BOOST_CHECK(db->ReadProfile("alice", pOut));
    BOOST_CHECK_EQUAL(pOut.reddId, "alice");
    BOOST_CHECK_EQUAL(pOut.displayName, "Alice Wonderland");
    BOOST_CHECK(pOut.verificationStatus == VERIFICATION_COMMUNITY);
    BOOST_CHECK_EQUAL(pOut.active, true);
    BOOST_CHECK_EQUAL(pOut.flags, 0x03);
}

BOOST_AUTO_TEST_CASE(profile_exists_true_false)
{
    BOOST_CHECK(!db->ExistsProfile("nobody"));
    ReddIDProfile p; p.reddId = "somebody"; p.verificationStatus = VERIFICATION_NONE;
    BOOST_CHECK(db->WriteProfile("somebody", p));
    BOOST_CHECK(db->ExistsProfile("somebody"));
    BOOST_CHECK(db->EraseProfile("somebody"));
    BOOST_CHECK(!db->ExistsProfile("somebody"));
}

BOOST_AUTO_TEST_CASE(profile_list_all)
{
    ReddIDProfile p1; p1.reddId = "user1"; p1.verificationStatus = VERIFICATION_NONE;
    ReddIDProfile p2; p2.reddId = "user2"; p2.verificationStatus = VERIFICATION_SELF;
    BOOST_CHECK(db->WriteProfile("user1", p1));
    BOOST_CHECK(db->WriteProfile("user2", p2));
    std::vector<std::string> reddIds;
    BOOST_CHECK(db->ListProfiles(reddIds));
    BOOST_CHECK_EQUAL(reddIds.size(), 2u);
}

// ===================================================================
// 7. Reputation DB
// ===================================================================

BOOST_AUTO_TEST_CASE(reputation_write_read_roundtrip)
{
    ReddIDReputation repIn;
    repIn.reddId = "alice";
    repIn.overallScore = 75.55;
    repIn.longevityScore = 80.25;
    repIn.reliabilityScore = 65.10;
    repIn.engagementScore = 90.00;
    repIn.verificationScore = 50.75;
    repIn.auctionScore = 85.33;
    repIn.lastCalculated = 1700600000;

    BOOST_CHECK(db->WriteReputation("alice", repIn));
    ReddIDReputation repOut;
    BOOST_CHECK(db->ReadReputation("alice", repOut));
    BOOST_CHECK_EQUAL(repOut.reddId, "alice");
    // Scores go through int32 * 100 roundtrip
    BOOST_CHECK(std::fabs(repOut.overallScore - 75.55) < 0.02);
    BOOST_CHECK(std::fabs(repOut.longevityScore - 80.25) < 0.02);
    BOOST_CHECK(std::fabs(repOut.engagementScore - 90.00) < 0.02);
    BOOST_CHECK_EQUAL(repOut.lastCalculated, 1700600000);
}

BOOST_AUTO_TEST_CASE(reputation_read_nonexistent)
{
    ReddIDReputation repOut;
    BOOST_CHECK(!db->ReadReputation("nonexistent", repOut));
}

// ===================================================================
// 8. Connection DB
// ===================================================================

BOOST_AUTO_TEST_CASE(connection_write_read_roundtrip)
{
    ReddIDConnection connIn;
    connIn.fromReddId = "alice";
    connIn.toReddId = "bob";
    connIn.connectionType = CONNECTION_FOLLOW;
    connIn.creationTime = 1700000000;
    connIn.lastInteraction = 1700500000;
    connIn.visibility = 0;
    connIn.metadata = "test connection";

    BOOST_CHECK(db->WriteConnection("alice", "bob", connIn));
    ReddIDConnection connOut;
    BOOST_CHECK(db->ReadConnection("alice", "bob", connOut));
    BOOST_CHECK_EQUAL(connOut.fromReddId, "alice");
    BOOST_CHECK_EQUAL(connOut.toReddId, "bob");
    BOOST_CHECK(connOut.connectionType == CONNECTION_FOLLOW);
    BOOST_CHECK_EQUAL(connOut.visibility, 0);
}

BOOST_AUTO_TEST_CASE(connection_list_for_reddid)
{
    ReddIDConnection c1; c1.fromReddId = "alice"; c1.toReddId = "bob"; c1.connectionType = CONNECTION_FOLLOW;
    ReddIDConnection c2; c2.fromReddId = "alice"; c2.toReddId = "carol"; c2.connectionType = CONNECTION_FRIEND;
    BOOST_CHECK(db->WriteConnection("alice", "bob", c1));
    BOOST_CHECK(db->WriteConnection("alice", "carol", c2));

    std::vector<ReddIDConnection> conns;
    BOOST_CHECK(db->ListConnections("alice", conns));
    BOOST_CHECK_EQUAL(conns.size(), 2u);
}

BOOST_AUTO_TEST_CASE(connection_erase_removes_entry)
{
    ReddIDConnection conn; conn.fromReddId = "alice"; conn.toReddId = "dave"; conn.connectionType = CONNECTION_BLOCK;
    BOOST_CHECK(db->WriteConnection("alice", "dave", conn));
    BOOST_CHECK(db->EraseConnection("alice", "dave"));
    ReddIDConnection connOut;
    BOOST_CHECK(!db->ReadConnection("alice", "dave", connOut));
}

// ===================================================================
// 9. Deposit lock DB
// ===================================================================

static CKeyID MakeKeyID(unsigned char fill)
{
    return CKeyID(uint160(std::vector<unsigned char>(20, fill)));
}

static DepositLock MakeDepositLock(const COutPoint& utxo, unsigned char fill)
{
    DepositLock lock;
    lock.utxo = utxo;
    lock.auctionId = uint256(std::vector<unsigned char>(32, fill));
    lock.bidId = uint256(std::vector<unsigned char>(32, fill + 1));
    lock.bidder = MakeKeyID(fill);
    lock.amount = fill * COIN;
    lock.lockHeight = 1000 + fill;
    lock.releaseHeight = 2000 + fill;
    lock.state = DEPOSIT_CONSUMED_WON;
    return lock;
}

static void CheckDepositLockEqual(const DepositLock& a, const DepositLock& b)
{
    BOOST_CHECK(a.utxo == b.utxo);
    BOOST_CHECK(a.auctionId == b.auctionId);
    BOOST_CHECK(a.bidId == b.bidId);
    BOOST_CHECK(a.bidder == b.bidder);
    BOOST_CHECK_EQUAL(a.amount, b.amount);
    BOOST_CHECK_EQUAL(a.lockHeight, b.lockHeight);
    BOOST_CHECK_EQUAL(a.releaseHeight, b.releaseHeight);
    BOOST_CHECK(a.state == b.state);
}

BOOST_AUTO_TEST_CASE(depositlock_write_read_exists_erase_list)
{
    const COutPoint utxo1(uint256S("d1d1000000000000000000000000000000000000000000000000000000000001"), 0);
    const COutPoint utxo2(uint256S("d1d1000000000000000000000000000000000000000000000000000000000001"), 7);
    const DepositLock lock1 = MakeDepositLock(utxo1, 0x11);
    const DepositLock lock2 = MakeDepositLock(utxo2, 0x22);

    BOOST_CHECK(!db->ExistsDepositLock(utxo1));
    DepositLock out;
    BOOST_CHECK(!db->ReadDepositLock(utxo1, out));

    BOOST_CHECK(db->WriteDepositLock(utxo1, lock1));
    BOOST_CHECK(db->WriteDepositLock(utxo2, lock2));
    BOOST_CHECK(db->ExistsDepositLock(utxo1));
    BOOST_CHECK(db->ExistsDepositLock(utxo2));

    // Same txid, different output index: two distinct records.
    BOOST_CHECK(db->ReadDepositLock(utxo1, out));
    CheckDepositLockEqual(out, lock1);
    BOOST_CHECK(db->ReadDepositLock(utxo2, out));
    CheckDepositLockEqual(out, lock2);

    std::vector<DepositLock> locks;
    BOOST_CHECK(db->ListAllDepositLocks(locks));
    BOOST_CHECK_EQUAL(locks.size(), 2U);

    BOOST_CHECK(db->EraseDepositLock(utxo1));
    BOOST_CHECK(!db->ExistsDepositLock(utxo1));
    BOOST_CHECK(db->ExistsDepositLock(utxo2));

    locks.clear();
    BOOST_CHECK(db->ListAllDepositLocks(locks));
    BOOST_CHECK_EQUAL(locks.size(), 1U);
    CheckDepositLockEqual(locks[0], lock2);
}

// ===================================================================
// 10. Auction config DB
// ===================================================================

static void CheckAuctionConfigEqual(const AuctionConfig& a, const AuctionConfig& b)
{
    BOOST_CHECK_EQUAL(a.configId, b.configId);
    BOOST_CHECK_EQUAL(a.depositPercentage, b.depositPercentage);
    BOOST_CHECK_EQUAL(a.settlementWindowBlocks, b.settlementWindowBlocks);
    BOOST_CHECK_EQUAL(a.antiSnipeThresholdSeconds, b.antiSnipeThresholdSeconds);
    BOOST_CHECK_EQUAL(a.antiSnipeExtensionSeconds, b.antiSnipeExtensionSeconds);
    BOOST_CHECK_EQUAL(a.minDurationDays, b.minDurationDays);
    BOOST_CHECK_EQUAL(a.maxDurationDays, b.maxDurationDays);
    BOOST_CHECK_EQUAL(a.minBidIncrementPct, b.minBidIncrementPct);
    BOOST_CHECK_EQUAL(a.communityFundAddress, b.communityFundAddress);
    BOOST_CHECK_EQUAL(a.updatedAt, b.updatedAt);
}

BOOST_AUTO_TEST_CASE(auctionconfig_write_read_erase_list)
{
    AuctionConfig in;
    in.configId = "custom";
    in.depositPercentage = 21;
    in.settlementWindowBlocks = 1234;
    in.antiSnipeThresholdSeconds = 111;
    in.antiSnipeExtensionSeconds = 222;
    in.minDurationDays = 2;
    in.maxDurationDays = 9;
    in.minBidIncrementPct = 6;
    in.communityFundAddress = "community-fund-address";
    in.updatedAt = 1700000123;

    AuctionConfig out;
    BOOST_CHECK(!db->ReadAuctionConfig("custom", out));
    BOOST_CHECK(db->WriteAuctionConfig("custom", in));
    BOOST_CHECK(db->WriteAuctionConfig("namespace", AuctionConfig::NamespaceDefaults()));
    BOOST_CHECK(db->ReadAuctionConfig("custom", out));
    CheckAuctionConfigEqual(out, in);
    BOOST_CHECK(db->ReadAuctionConfig("namespace", out));
    CheckAuctionConfigEqual(out, AuctionConfig::NamespaceDefaults());

    std::vector<AuctionConfig> configs;
    BOOST_CHECK(db->ListAuctionConfigs(configs));
    BOOST_CHECK_EQUAL(configs.size(), 2U);

    BOOST_CHECK(db->EraseAuctionConfig("custom"));
    BOOST_CHECK(!db->ReadAuctionConfig("custom", out));
    configs.clear();
    BOOST_CHECK(db->ListAuctionConfigs(configs));
    BOOST_CHECK_EQUAL(configs.size(), 1U);
    BOOST_CHECK_EQUAL(configs[0].configId, "namespace");
}

// ===================================================================
// 11. Namespace resolution DB
// ===================================================================

BOOST_AUTO_TEST_CASE(resolution_forward_and_reverse)
{
    std::string out;
    BOOST_CHECK(!db->ReadResolution("alice", "redd", out));
    BOOST_CHECK(!db->ReadResolutionByUserID("alice1", "redd", out));

    BOOST_CHECK(db->WriteResolution("alice", "redd", "alice1"));
    BOOST_CHECK(db->WriteResolution("alice", "social", "alice2"));
    BOOST_CHECK(db->WriteResolution("bob", "redd", "bob1"));

    BOOST_CHECK(db->ReadResolution("alice", "redd", out));
    BOOST_CHECK_EQUAL(out, "alice1");
    BOOST_CHECK(db->ReadResolution("alice", "social", out));
    BOOST_CHECK_EQUAL(out, "alice2");
    BOOST_CHECK(db->ReadResolutionByUserID("alice1", "redd", out));
    BOOST_CHECK_EQUAL(out, "alice");
    BOOST_CHECK(db->ReadResolutionByUserID("bob1", "redd", out));
    BOOST_CHECK_EQUAL(out, "bob");
    // The reverse lookup is per namespace.
    BOOST_CHECK(!db->ReadResolutionByUserID("alice1", "social", out));

    std::map<std::string, std::string> resolutions;
    BOOST_CHECK(db->ListResolutions("alice", resolutions));
    const std::map<std::string, std::string> expected{{"redd", "alice1"}, {"social", "alice2"}};
    BOOST_CHECK(resolutions == expected);

    // Erasing removes the forward entry and its reverse entry, and nothing else.
    BOOST_CHECK(db->EraseResolution("alice", "redd"));
    BOOST_CHECK(!db->ReadResolution("alice", "redd", out));
    BOOST_CHECK(!db->ReadResolutionByUserID("alice1", "redd", out));
    BOOST_CHECK(db->ReadResolution("alice", "social", out));
    BOOST_CHECK(db->ReadResolutionByUserID("alice2", "social", out));
    BOOST_CHECK(db->ReadResolution("bob", "redd", out));

    // Erasing an entry that does not exist reports success.
    BOOST_CHECK(db->EraseResolution("nobody", "redd"));
}

BOOST_AUTO_TEST_CASE(resolution_overwrite_keeps_stale_reverse_entry)
{
    // Current behaviour: WriteResolution does not remove the reverse entry of
    // the user ID it replaces, so the old user ID keeps resolving.
    BOOST_CHECK(db->WriteResolution("alice", "redd", "old"));
    BOOST_CHECK(db->WriteResolution("alice", "redd", "new"));

    std::string out;
    BOOST_CHECK(db->ReadResolution("alice", "redd", out));
    BOOST_CHECK_EQUAL(out, "new");
    BOOST_CHECK(db->ReadResolutionByUserID("new", "redd", out));
    BOOST_CHECK_EQUAL(out, "alice");
    BOOST_CHECK(db->ReadResolutionByUserID("old", "redd", out));
    BOOST_CHECK_EQUAL(out, "alice");

    // EraseResolution only removes the reverse entry of the current user ID.
    BOOST_CHECK(db->EraseResolution("alice", "redd"));
    BOOST_CHECK(!db->ReadResolutionByUserID("new", "redd", out));
    BOOST_CHECK(db->ReadResolutionByUserID("old", "redd", out));
    BOOST_CHECK_EQUAL(out, "alice");
}

// ===================================================================
// 12. Remaining list/query methods
// ===================================================================

BOOST_AUTO_TEST_CASE(profile_list_by_owner)
{
    const CKeyID owner1 = MakeKeyID(0x01);
    const CKeyID owner2 = MakeKeyID(0x02);
    ReddIDProfile p1; p1.reddId = "one"; p1.owner = owner1;
    ReddIDProfile p2; p2.reddId = "two"; p2.owner = owner2;
    ReddIDProfile p3; p3.reddId = "three"; p3.owner = owner1;
    BOOST_CHECK(db->WriteProfile("one", p1));
    BOOST_CHECK(db->WriteProfile("two", p2));
    BOOST_CHECK(db->WriteProfile("three", p3));

    std::vector<std::string> ids;
    BOOST_CHECK(db->ListProfilesByOwner(owner1, ids));
    BOOST_CHECK((std::set<std::string>(ids.begin(), ids.end()) == std::set<std::string>{"one", "three"}));
    BOOST_CHECK_EQUAL(ids.size(), 2U);

    ids.clear();
    BOOST_CHECK(db->ListProfilesByOwner(MakeKeyID(0x03), ids));
    BOOST_CHECK(ids.empty());
}

BOOST_AUTO_TEST_CASE(reputation_history_current_behaviour)
{
    ReddIDReputation rep;
    rep.reddId = "alice";
    rep.overallScore = 12.5;
    rep.lastCalculated = 1000;
    BOOST_CHECK(db->WriteReputation("alice", rep));

    // There is no history storage: the single stored record is returned, and
    // only when its lastCalculated is at or before startTime.
    std::vector<ReddIDReputation> history;
    BOOST_CHECK(!db->GetReputationHistory("alice", history, 999, 10));
    BOOST_CHECK(history.empty());
    BOOST_CHECK(db->GetReputationHistory("alice", history, 1000, 10));
    BOOST_CHECK_EQUAL(history.size(), 1U);
    BOOST_CHECK_EQUAL(history[0].reddId, "alice");
    BOOST_CHECK_EQUAL(history[0].overallScore, 12.5);

    // The count argument is not used.
    history.clear();
    BOOST_CHECK(db->GetReputationHistory("alice", history, 5000, 0));
    BOOST_CHECK_EQUAL(history.size(), 1U);

    history.clear();
    BOOST_CHECK(!db->GetReputationHistory("nobody", history, 5000, 10));
    BOOST_CHECK(history.empty());

    // The return value only reflects whether the output vector is non-empty.
    history.push_back(rep);
    BOOST_CHECK(db->GetReputationHistory("nobody", history, 5000, 10));
    BOOST_CHECK_EQUAL(history.size(), 1U);
}

BOOST_AUTO_TEST_CASE(list_methods_append_and_order_by_key_length)
{
    // Keys are stored as serialized strings (length prefix first), so
    // iteration order is by key length, then bytewise.
    // Current behaviour: List* methods append to the output vector without
    // clearing it, so "preexisting" survives the call.
    NamespaceInfo ns;
    BOOST_CHECK(db->WriteNamespace("alpha", ns));
    BOOST_CHECK(db->WriteNamespace("zz", ns));
    BOOST_CHECK(db->WriteNamespace("beta", ns));

    std::vector<std::string> ids{"preexisting"};
    BOOST_CHECK(db->ListNamespaces(ids));
    const std::vector<std::string> expected{"preexisting", "zz", "beta", "alpha"};
    BOOST_CHECK_EQUAL_COLLECTIONS(ids.begin(), ids.end(), expected.begin(), expected.end());
}

// ===================================================================
// 13. Every field survives a write/read round trip
// ===================================================================

BOOST_AUTO_TEST_CASE(namespace_roundtrip_all_fields)
{
    NamespaceInfo in;
    in.id = "full";
    in.owner = MakeKeyID(0xA1);
    in.allowNumbers = true;
    in.allowHyphens = false;
    in.allowUnderscores = true;
    in.minLength = 4;
    in.maxLength = 21;
    in.renewalPeriod = 366;
    in.gracePeriod = 31;
    in.namespaceRevenuePct = 11;
    in.burnPct = 69;
    in.nodePct = 14;
    in.devPct = 6;
    in.minAuctionDuration = 2;
    in.maxAuctionDuration = 8;
    in.minBidIncrement = 7.55;
    in.configHash = uint256S("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    in.lastUpdated = 1700000001;
    in.expiration = 1800000002;
    BOOST_CHECK(db->WriteNamespace("full", in));

    NamespaceInfo out;
    BOOST_CHECK(db->ReadNamespace("full", out));
    BOOST_CHECK_EQUAL(out.id, in.id);
    BOOST_CHECK(out.owner == in.owner);
    BOOST_CHECK_EQUAL(out.allowNumbers, true);
    BOOST_CHECK_EQUAL(out.allowHyphens, false);
    BOOST_CHECK_EQUAL(out.allowUnderscores, true);
    BOOST_CHECK_EQUAL(out.minLength, 4);
    BOOST_CHECK_EQUAL(out.maxLength, 21);
    BOOST_CHECK_EQUAL(out.renewalPeriod, 366);
    BOOST_CHECK_EQUAL(out.gracePeriod, 31);
    BOOST_CHECK_EQUAL(out.namespaceRevenuePct, 11);
    BOOST_CHECK_EQUAL(out.burnPct, 69);
    BOOST_CHECK_EQUAL(out.nodePct, 14);
    BOOST_CHECK_EQUAL(out.devPct, 6);
    BOOST_CHECK_EQUAL(out.minAuctionDuration, 2);
    BOOST_CHECK_EQUAL(out.maxAuctionDuration, 8);
    // Stored as int32(755), read back as 755 / 100.0, which is the double 7.55.
    BOOST_CHECK_EQUAL(out.minBidIncrement, 7.55);
    BOOST_CHECK(out.configHash == in.configHash);
    BOOST_CHECK_EQUAL(out.lastUpdated, 1700000001);
    BOOST_CHECK_EQUAL(out.expiration, 1800000002);
}

BOOST_AUTO_TEST_CASE(pricingtier_roundtrip_all_fields)
{
    PricingTier in;
    in.namespaceId = "full";
    in.minLength = 7;
    in.minPrice = 123456789 * COIN + 1;
    BOOST_CHECK(db->WritePricingTier(in));

    std::vector<PricingTier> tiers;
    BOOST_CHECK(db->ReadPricingTiers("full", tiers));
    BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
    BOOST_CHECK_EQUAL(tiers[0].namespaceId, "full");
    BOOST_CHECK_EQUAL(tiers[0].minLength, 7);
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 123456789 * COIN + 1);
}

BOOST_AUTO_TEST_CASE(userid_roundtrip_all_fields)
{
    UserIDInfo in;
    in.name = "alice";
    in.namespaceId = "full";
    in.owner = MakeKeyID(0xB2);
    in.registrationTime = 1700000003;
    in.expirationTime = 1750000004;
    in.lastTransaction = 1700500005;
    in.transactionCount = 43;
    in.metadataHash = uint256S("1111111111111111111111111111111111111111111111111111111111111112");
    BOOST_CHECK(db->WriteUserID("alice", "full", in));

    UserIDInfo out;
    BOOST_CHECK(db->ReadUserID("alice", "full", out));
    BOOST_CHECK_EQUAL(out.name, "alice");
    BOOST_CHECK_EQUAL(out.namespaceId, "full");
    BOOST_CHECK(out.owner == in.owner);
    BOOST_CHECK_EQUAL(out.registrationTime, 1700000003);
    BOOST_CHECK_EQUAL(out.expirationTime, 1750000004);
    BOOST_CHECK_EQUAL(out.lastTransaction, 1700500005);
    BOOST_CHECK_EQUAL(out.transactionCount, 43);
    BOOST_CHECK(out.metadataHash == in.metadataHash);
}

BOOST_AUTO_TEST_CASE(auction_roundtrip_all_fields)
{
    AuctionInfo in;
    in.auctionId = uint256S("aaaa000000000000000000000000000000000000000000000000000000000011");
    in.name = "premium";
    in.namespaceId = "full";
    in.creator = MakeKeyID(0xC3);
    in.startTime = 1700000006;
    in.endTime = 1700100007;
    in.reservePrice = 5000 * COIN + 1;
    in.currentBid = 7500 * COIN + 2;
    in.currentBidder = MakeKeyID(0xC4);
    in.depositAmount = 500 * COIN + 3;
    in.state = AUCTION_RE_AUCTION;
    in.type = AUCTION_RENEWAL;
    in.metadataHash = uint256S("2222222222222222222222222222222222222222222222222222222222222201");
    in.blockHeight = 100001;
    in.txid = uint256S("3333333333333333333333333333333333333333333333333333333333333302");
    in.settlementDeadlineHeight = 104321;
    in.finalizeTxid = uint256S("4444444444444444444444444444444444444444444444444444444444444403");
    in.settleTxid = uint256S("5555555555555555555555555555555555555555555555555555555555555504");
    in.totalDepositsLocked = 900 * COIN + 4;
    in.reAuctionCount = 3;
    in.previousAuctionId = uint256S("6666666666666666666666666666666666666666666666666666666666666605");
    BOOST_CHECK(db->WriteAuction(in.auctionId, in));

    AuctionInfo out;
    BOOST_CHECK(db->ReadAuction(in.auctionId, out));
    BOOST_CHECK(out.auctionId == in.auctionId);
    BOOST_CHECK_EQUAL(out.name, "premium");
    BOOST_CHECK_EQUAL(out.namespaceId, "full");
    BOOST_CHECK(out.creator == in.creator);
    BOOST_CHECK_EQUAL(out.startTime, 1700000006);
    BOOST_CHECK_EQUAL(out.endTime, 1700100007);
    BOOST_CHECK_EQUAL(out.reservePrice, 5000 * COIN + 1);
    BOOST_CHECK_EQUAL(out.currentBid, 7500 * COIN + 2);
    BOOST_CHECK(out.currentBidder == in.currentBidder);
    BOOST_CHECK_EQUAL(out.depositAmount, 500 * COIN + 3);
    BOOST_CHECK(out.state == AUCTION_RE_AUCTION);
    BOOST_CHECK(out.type == AUCTION_RENEWAL);
    BOOST_CHECK(out.metadataHash == in.metadataHash);
    BOOST_CHECK_EQUAL(out.blockHeight, 100001);
    BOOST_CHECK(out.txid == in.txid);
    BOOST_CHECK_EQUAL(out.settlementDeadlineHeight, 104321);
    BOOST_CHECK(out.finalizeTxid == in.finalizeTxid);
    BOOST_CHECK(out.settleTxid == in.settleTxid);
    BOOST_CHECK_EQUAL(out.totalDepositsLocked, 900 * COIN + 4);
    BOOST_CHECK_EQUAL(out.reAuctionCount, 3);
    BOOST_CHECK(out.previousAuctionId == in.previousAuctionId);
}

BOOST_AUTO_TEST_CASE(bid_roundtrip_all_fields)
{
    BidInfo in;
    in.bidId = uint256S("eeee000000000000000000000000000000000000000000000000000000000011");
    in.auctionId = uint256S("ffff000000000000000000000000000000000000000000000000000000000012");
    in.bidder = MakeKeyID(0xD5);
    in.bidderReddId = "alice.redd";
    in.bidAmount = 10000 * COIN + 1;
    in.depositAmount = 1000 * COIN + 2;
    in.bidTime = 1700050008;
    in.proofOfFunds = uint256S("7777777777777777777777777777777777777777777777777777777777777706");
    in.txid = uint256S("8888888888888888888888888888888888888888888888888888888888888807");
    in.isWinner = true;
    in.refunded = true;
    in.depositUtxo = COutPoint(uint256S("9999999999999999999999999999999999999999999999999999999999999908"), 5);
    in.depositLockHeight = 2001;
    in.depositReleaseHeight = 2002;
    in.depositState = DEPOSIT_FORFEITED;
    BOOST_CHECK(db->WriteBid(in.bidId, in));

    BidInfo out;
    BOOST_CHECK(db->ReadBid(in.bidId, out));
    BOOST_CHECK(out.bidId == in.bidId);
    BOOST_CHECK(out.auctionId == in.auctionId);
    BOOST_CHECK(out.bidder == in.bidder);
    BOOST_CHECK_EQUAL(out.bidderReddId, "alice.redd");
    BOOST_CHECK_EQUAL(out.bidAmount, 10000 * COIN + 1);
    BOOST_CHECK_EQUAL(out.depositAmount, 1000 * COIN + 2);
    BOOST_CHECK_EQUAL(out.bidTime, 1700050008);
    BOOST_CHECK(out.proofOfFunds == in.proofOfFunds);
    BOOST_CHECK(out.txid == in.txid);
    BOOST_CHECK_EQUAL(out.isWinner, true);
    BOOST_CHECK_EQUAL(out.refunded, true);
    BOOST_CHECK(out.depositUtxo == in.depositUtxo);
    BOOST_CHECK_EQUAL(out.depositLockHeight, 2001);
    BOOST_CHECK_EQUAL(out.depositReleaseHeight, 2002);
    BOOST_CHECK(out.depositState == DEPOSIT_FORFEITED);

    // UpdateBidStatus rewrites the two flags and leaves everything else alone.
    BOOST_CHECK(db->UpdateBidStatus(in.bidId, false, false));
    BidInfo updated;
    BOOST_CHECK(db->ReadBid(in.bidId, updated));
    BOOST_CHECK_EQUAL(updated.isWinner, false);
    BOOST_CHECK_EQUAL(updated.refunded, false);
    BOOST_CHECK(updated.depositUtxo == in.depositUtxo);
    BOOST_CHECK(updated.depositState == DEPOSIT_FORFEITED);
    BOOST_CHECK_EQUAL(updated.bidAmount, in.bidAmount);

    // UpdateBidStatus fails for a bid that is not stored.
    BOOST_CHECK(!db->UpdateBidStatus(uint256S("01"), true, true));
}

BOOST_AUTO_TEST_CASE(profile_roundtrip_all_fields)
{
    ReddIDProfile in;
    in.reddId = "alice";
    in.owner = MakeKeyID(0xE6);
    in.displayName = "Alice Wonderland";
    in.avatarHash = uint256S("aaaa111111111111111111111111111111111111111111111111111111111109");
    in.bio = "Blockchain enthusiast";
    in.emailHash = uint256S("bbbb22222222222222222222222222222222222222222222222222222222220a");
    in.socialData = "{\"x\":\"@alice\"}";
    in.messagingPubkey = std::vector<unsigned char>{0x02, 0x00, 0xff, 0x10, 0x20};
    in.verificationStatus = VERIFICATION_OFFICIAL;
    in.creationTime = 1700000009;
    in.lastUpdated = 1700500010;
    in.expirationTime = 1800000011;
    in.lastActivityTime = 1700600012;
    in.active = false;
    in.flags = 0xA5;
    BOOST_CHECK(db->WriteProfile("alice", in));

    ReddIDProfile out;
    BOOST_CHECK(db->ReadProfile("alice", out));
    BOOST_CHECK_EQUAL(out.reddId, "alice");
    BOOST_CHECK(out.owner == in.owner);
    BOOST_CHECK_EQUAL(out.displayName, "Alice Wonderland");
    BOOST_CHECK(out.avatarHash == in.avatarHash);
    BOOST_CHECK_EQUAL(out.bio, "Blockchain enthusiast");
    BOOST_CHECK(out.emailHash == in.emailHash);
    BOOST_CHECK_EQUAL(out.socialData, in.socialData);
    BOOST_CHECK(out.messagingPubkey == in.messagingPubkey);
    BOOST_CHECK(out.verificationStatus == VERIFICATION_OFFICIAL);
    BOOST_CHECK_EQUAL(out.creationTime, 1700000009);
    BOOST_CHECK_EQUAL(out.lastUpdated, 1700500010);
    BOOST_CHECK_EQUAL(out.expirationTime, 1800000011);
    BOOST_CHECK_EQUAL(out.lastActivityTime, 1700600012);
    BOOST_CHECK_EQUAL(out.active, false);
    BOOST_CHECK_EQUAL(out.flags, 0xA5);
}

BOOST_AUTO_TEST_CASE(connection_roundtrip_all_fields)
{
    ReddIDConnection in;
    in.fromReddId = "alice";
    in.toReddId = "bob";
    in.connectionType = CONNECTION_BLOCK;
    in.creationTime = 1700000013;
    in.lastInteraction = 1700500014;
    in.visibility = 2;
    in.metadata = "test connection";
    in.txid = uint256S("cccc33333333333333333333333333333333333333333333333333333333330b");
    BOOST_CHECK(db->WriteConnection("alice", "bob", in));

    ReddIDConnection out;
    BOOST_CHECK(db->ReadConnection("alice", "bob", out));
    BOOST_CHECK_EQUAL(out.fromReddId, "alice");
    BOOST_CHECK_EQUAL(out.toReddId, "bob");
    BOOST_CHECK(out.connectionType == CONNECTION_BLOCK);
    BOOST_CHECK_EQUAL(out.creationTime, 1700000013);
    BOOST_CHECK_EQUAL(out.lastInteraction, 1700500014);
    BOOST_CHECK_EQUAL(out.visibility, 2);
    BOOST_CHECK_EQUAL(out.metadata, "test connection");
    BOOST_CHECK(out.txid == in.txid);

    // The direction is part of the key.
    BOOST_CHECK(!db->ReadConnection("bob", "alice", out));
}

BOOST_AUTO_TEST_CASE(reputation_roundtrip_all_fields)
{
    ReddIDReputation in;
    in.reddId = "alice";
    in.overallScore = 75.55;
    in.longevityScore = 80.25;
    in.reliabilityScore = 65.10;
    in.engagementScore = 90.00;
    in.verificationScore = 50.75;
    in.auctionScore = 85.33;
    in.lastCalculated = 1700600015;
    in.calculationProof = uint256S("dddd44444444444444444444444444444444444444444444444444444444440c");
    in.calculatorSignatures = "[\"sig1\",\"sig2\"]";
    BOOST_CHECK(db->WriteReputation("alice", in));

    ReddIDReputation out;
    BOOST_CHECK(db->ReadReputation("alice", out));
    BOOST_CHECK_EQUAL(out.reddId, "alice");
    // Two-decimal values survive exactly: n / 100.0 is the double nearest to
    // the decimal literal.
    BOOST_CHECK_EQUAL(out.overallScore, 75.55);
    BOOST_CHECK_EQUAL(out.longevityScore, 80.25);
    BOOST_CHECK_EQUAL(out.reliabilityScore, 65.10);
    BOOST_CHECK_EQUAL(out.engagementScore, 90.00);
    BOOST_CHECK_EQUAL(out.verificationScore, 50.75);
    BOOST_CHECK_EQUAL(out.auctionScore, 85.33);
    BOOST_CHECK_EQUAL(out.lastCalculated, 1700600015);
    BOOST_CHECK(out.calculationProof == in.calculationProof);
    BOOST_CHECK_EQUAL(out.calculatorSignatures, in.calculatorSignatures);
}

BOOST_AUTO_TEST_CASE(scaled_int_fields_round_to_two_decimals)
{
    // Doubles are stored as int32(value * 100 + 0.5): rounded to two decimals
    // for non-negative values.
    ReddIDReputation in;
    in.reddId = "round";
    in.overallScore = 5.126;       // -> 513
    in.longevityScore = 5.124;     // -> 512
    in.reliabilityScore = 0.004;   // -> 0
    in.engagementScore = 100.0;    // -> 10000
    in.verificationScore = 0.005;  // -> 1
    // Current behaviour: the conversion truncates toward zero after adding 0.5, so a negative
    // value is not rounded to nearest: -1.5 * 100 + 0.5 = -149.5 -> -149.
    in.auctionScore = -1.5;
    BOOST_CHECK(db->WriteReputation("round", in));

    ReddIDReputation out;
    BOOST_CHECK(db->ReadReputation("round", out));
    BOOST_CHECK_EQUAL(out.overallScore, 5.13);
    BOOST_CHECK_EQUAL(out.longevityScore, 5.12);
    BOOST_CHECK_EQUAL(out.reliabilityScore, 0.0);
    BOOST_CHECK_EQUAL(out.engagementScore, 100.0);
    BOOST_CHECK_EQUAL(out.verificationScore, 0.01);
    BOOST_CHECK_EQUAL(out.auctionScore, -1.49);

    NamespaceInfo nsIn;
    nsIn.id = "round";
    nsIn.minBidIncrement = 2.499;  // -> 250
    BOOST_CHECK(db->WriteNamespace("round", nsIn));
    NamespaceInfo nsOut;
    BOOST_CHECK(db->ReadNamespace("round", nsOut));
    BOOST_CHECK_EQUAL(nsOut.minBidIncrement, 2.5);
}

BOOST_AUTO_TEST_CASE(enum_fields_are_stored_as_unvalidated_bytes)
{
    // Every enumerator of each enum survives.
    for (uint8_t s = AUCTION_PENDING; s <= AUCTION_CANCELED; ++s) {
        AuctionInfo a;
        a.auctionId = uint256(std::vector<unsigned char>(32, s + 1));
        a.state = static_cast<AuctionState>(s);
        a.type = static_cast<AuctionType>(s % 4);
        BOOST_CHECK(db->WriteAuction(a.auctionId, a));
        AuctionInfo out;
        BOOST_CHECK(db->ReadAuction(a.auctionId, out));
        BOOST_CHECK(out.state == static_cast<AuctionState>(s));
        BOOST_CHECK(out.type == static_cast<AuctionType>(s % 4));
    }
    for (uint8_t s = DEPOSIT_LOCKED_ACTIVE; s <= DEPOSIT_FORFEITED; ++s) {
        const COutPoint utxo(uint256S("e0"), s);
        DepositLock lock;
        lock.utxo = utxo;
        lock.state = static_cast<DepositState>(s);
        BOOST_CHECK(db->WriteDepositLock(utxo, lock));
        DepositLock lockOut;
        BOOST_CHECK(db->ReadDepositLock(utxo, lockOut));
        BOOST_CHECK(lockOut.state == static_cast<DepositState>(s));

        BidInfo bid;
        bid.bidId = uint256(std::vector<unsigned char>(32, 0x40 + s));
        bid.depositState = static_cast<DepositState>(s);
        BOOST_CHECK(db->WriteBid(bid.bidId, bid));
        BidInfo bidOut;
        BOOST_CHECK(db->ReadBid(bid.bidId, bidOut));
        BOOST_CHECK(bidOut.depositState == static_cast<DepositState>(s));

        ReddIDProfile profile;
        profile.verificationStatus = static_cast<VerificationStatus>(s);
        BOOST_CHECK(db->WriteProfile("enum", profile));
        ReddIDProfile profileOut;
        BOOST_CHECK(db->ReadProfile("enum", profileOut));
        BOOST_CHECK(profileOut.verificationStatus == static_cast<VerificationStatus>(s));

        ReddIDConnection conn;
        conn.connectionType = static_cast<SocialConnectionType>(s);
        BOOST_CHECK(db->WriteConnection("enum", "target", conn));
        ReddIDConnection connOut;
        BOOST_CHECK(db->ReadConnection("enum", "target", connOut));
        BOOST_CHECK(connOut.connectionType == static_cast<SocialConnectionType>(s));
    }

    // Current behaviour: a byte outside the enumerator range is read back unchanged; nothing
    // validates it on either side.
    AuctionInfo odd;
    odd.auctionId = uint256S("0dd0");
    odd.state = static_cast<AuctionState>(200);
    odd.type = static_cast<AuctionType>(201);
    BOOST_CHECK(db->WriteAuction(odd.auctionId, odd));
    AuctionInfo oddOut;
    BOOST_CHECK(db->ReadAuction(odd.auctionId, oddOut));
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(oddOut.state), 200);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(oddOut.type), 201);

    std::vector<uint256> ids;
    BOOST_CHECK(db->ListAuctionsByState(static_cast<AuctionState>(200), ids));
    BOOST_REQUIRE_EQUAL(ids.size(), 1U);
    BOOST_CHECK(ids[0] == odd.auctionId);
}

// ===================================================================
// 14. On-disk format: key strings and value bytes
// ===================================================================

template <typename T>
static std::string SerializedHex(const T& obj)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << obj;
    return HexStr(MakeUCharSpan(ss));
}

BOOST_AUTO_TEST_CASE(value_serialization_bytes)
{
    PricingTier tier;
    tier.namespaceId = "ab";
    tier.minLength = 3;
    tier.minPrice = 1;
    BOOST_CHECK_EQUAL(SerializedHex(tier),
        "026162"             // namespaceId
        "03000000"           // minLength
        "0100000000000000"); // minPrice

    ReddIDReputation rep;
    rep.reddId = "a";
    rep.overallScore = 1.0;
    rep.longevityScore = 2.55;
    rep.reliabilityScore = 0.0;
    rep.engagementScore = 100.0;
    rep.verificationScore = 0.01;
    rep.auctionScore = 50.5;
    rep.lastCalculated = 1;
    BOOST_CHECK_EQUAL(SerializedHex(rep),
        "0161"               // reddId
        "64000000"           // overallScore 100
        "ff000000"           // longevityScore 255
        "00000000"           // reliabilityScore 0
        "10270000"           // engagementScore 10000
        "01000000"           // verificationScore 1
        "ba130000"           // auctionScore 5050
        "0100000000000000"   // lastCalculated
        "0000000000000000000000000000000000000000000000000000000000000000" // calculationProof
        "00");               // calculatorSignatures

    DepositLock lock;
    lock.utxo = COutPoint(uint256(), 2);
    lock.amount = 3;
    lock.lockHeight = 4;
    lock.releaseHeight = 5;
    lock.state = DEPOSIT_FORFEITED;
    BOOST_CHECK_EQUAL(SerializedHex(lock),
        "0000000000000000000000000000000000000000000000000000000000000000" "02000000" // utxo
        "0000000000000000000000000000000000000000000000000000000000000000" // auctionId
        "0000000000000000000000000000000000000000000000000000000000000000" // bidId
        "0000000000000000000000000000000000000000" // bidder
        "0300000000000000"   // amount
        "04000000"           // lockHeight
        "05000000"           // releaseHeight
        "03");               // state

    // The profile record: enum as one byte, then the four timestamps with
    // lastActivityTime last, then active and flags.
    ReddIDProfile profile;
    profile.reddId = "p";
    profile.messagingPubkey = std::vector<unsigned char>{0xab};
    profile.verificationStatus = VERIFICATION_COMMUNITY;
    profile.creationTime = 1;
    profile.lastUpdated = 2;
    profile.expirationTime = 3;
    profile.lastActivityTime = 4;
    profile.active = true;
    profile.flags = 0x81;
    BOOST_CHECK_EQUAL(SerializedHex(profile),
        "0170"               // reddId
        "0000000000000000000000000000000000000000" // owner
        "00"                 // displayName
        "0000000000000000000000000000000000000000000000000000000000000000" // avatarHash
        "00"                 // bio
        "0000000000000000000000000000000000000000000000000000000000000000" // emailHash
        "00"                 // socialData
        "01ab"               // messagingPubkey
        "02"                 // verificationStatus
        "0100000000000000"   // creationTime
        "0200000000000000"   // lastUpdated
        "0300000000000000"   // expirationTime
        "0400000000000000"   // lastActivityTime
        "01"                 // active
        "81");               // flags
}

BOOST_AUTO_TEST_CASE(key_strings)
{
    // The keys are the on-disk format; look each record up by its literal key
    // through the underlying wrapper.
    const std::string hash1 = "00000000000000000000000000000000000000000000000000000000000000a1";
    const uint256 id1 = uint256S(hash1);

    BOOST_CHECK(db->WriteNamespace("redd", NamespaceInfo()));
    BOOST_CHECK(db->Exists(std::string("ns-redd")));

    PricingTier tier; tier.namespaceId = "redd"; tier.minLength = 12;
    BOOST_CHECK(db->WritePricingTier(tier));
    BOOST_CHECK(db->Exists(std::string("tier-redd-12")));
    PricingTier negTier; negTier.namespaceId = "redd"; negTier.minLength = -1;
    BOOST_CHECK(db->WritePricingTier(negTier));
    BOOST_CHECK(db->Exists(std::string("tier-redd--1")));
    BOOST_CHECK(db->ErasePricingTier("redd", -1));
    BOOST_CHECK(!db->Exists(std::string("tier-redd--1")));

    BOOST_CHECK(db->WriteUserID("alice", "redd", UserIDInfo()));
    BOOST_CHECK(db->Exists(std::string("id-alice.redd")));

    BOOST_CHECK(db->WriteAuction(id1, AuctionInfo()));
    BOOST_CHECK(db->Exists(std::string("auc-") + hash1));

    BOOST_CHECK(db->WriteBid(id1, BidInfo()));
    BOOST_CHECK(db->Exists(std::string("bid-") + hash1));

    BOOST_CHECK(db->WriteProfile("alice", ReddIDProfile()));
    BOOST_CHECK(db->Exists(std::string("profile-alice")));

    BOOST_CHECK(db->WriteReputation("alice", ReddIDReputation()));
    BOOST_CHECK(db->Exists(std::string("rep-alice")));

    BOOST_CHECK(db->WriteConnection("alice", "bob", ReddIDConnection()));
    BOOST_CHECK(db->Exists(std::string("conn-alice-bob")));

    BOOST_CHECK(db->WriteDepositLock(COutPoint(id1, 0), DepositLock()));
    BOOST_CHECK(db->Exists(std::string("dep-") + hash1 + "-0"));
    BOOST_CHECK(db->WriteDepositLock(COutPoint(id1, 4294967295U), DepositLock()));
    BOOST_CHECK(db->Exists(std::string("dep-") + hash1 + "-4294967295"));

    BOOST_CHECK(db->WriteAuctionConfig("userid", AuctionConfig::UserIDDefaults()));
    BOOST_CHECK(db->Exists(std::string("aconf-userid")));

    BOOST_CHECK(db->WriteResolution("alice", "redd", "alice1"));
    BOOST_CHECK(db->Exists(std::string("res-reddid-alice-redd")));
    BOOST_CHECK(db->Exists(std::string("res-userid-alice1-redd")));

    // Nothing else was written (no obfuscation key either).
    size_t count = 0;
    std::unique_ptr<CDBIterator> it(db->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++count;
    BOOST_CHECK_EQUAL(count, 13U);
}

// ===================================================================
// 15. Key families share one keyspace
// ===================================================================

template <typename T>
static std::set<T> ToSet(const std::vector<T>& v)
{
    return std::set<T>(v.begin(), v.end());
}

BOOST_AUTO_TEST_CASE(list_methods_return_only_their_own_family)
{
    using StrSet = std::set<std::string>;
    const uint256 idA = uint256S("00000000000000000000000000000000000000000000000000000000000000a1");
    const uint256 idB = uint256S("00000000000000000000000000000000000000000000000000000000000000b2");
    const uint256 idC = uint256S("00000000000000000000000000000000000000000000000000000000000000c3");
    const CKeyID owner1 = MakeKeyID(0x01);
    const CKeyID owner2 = MakeKeyID(0x02);

    // Namespaces, including ids that look like other families' prefixes.
    for (const std::string& id : {"redd", "a", "a-b", "tier-a"}) {
        NamespaceInfo ns; ns.id = id;
        BOOST_CHECK(db->WriteNamespace(id, ns));
    }
    // Pricing tiers.
    for (const auto& [id, len] : std::vector<std::pair<std::string, int>>{{"redd", 1}, {"a", 3}, {"a", 5}, {"a-b", 3}}) {
        PricingTier tier; tier.namespaceId = id; tier.minLength = len; tier.minPrice = len * COIN;
        BOOST_CHECK(db->WritePricingTier(tier));
    }
    // User IDs.
    for (const auto& [name, ns] : std::vector<std::pair<std::string, std::string>>{{"alice", "redd"}, {"bob", "redd"}, {"carol", "a-b"}, {"ns-x", "a"}}) {
        UserIDInfo uid; uid.name = name; uid.namespaceId = ns;
        BOOST_CHECK(db->WriteUserID(name, ns, uid));
    }
    // Auctions and bids; bid ids deliberately reuse the auction ids.
    {
        AuctionInfo a1; a1.auctionId = idA; a1.state = AUCTION_ACTIVE;
        AuctionInfo a2; a2.auctionId = idB; a2.state = AUCTION_ENDED;
        BOOST_CHECK(db->WriteAuction(idA, a1));
        BOOST_CHECK(db->WriteAuction(idB, a2));
        BidInfo b1; b1.bidId = idA; b1.auctionId = idA;
        BidInfo b2; b2.bidId = idB; b2.auctionId = idB;
        BidInfo b3; b3.bidId = idC; b3.auctionId = idA;
        BOOST_CHECK(db->WriteBid(idA, b1));
        BOOST_CHECK(db->WriteBid(idB, b2));
        BOOST_CHECK(db->WriteBid(idC, b3));
    }
    // Profiles and reputations, with ids that look like the other family.
    {
        ReddIDProfile p1; p1.reddId = "alice"; p1.owner = owner1;
        ReddIDProfile p2; p2.reddId = "bob"; p2.owner = owner2;
        ReddIDProfile p3; p3.reddId = "rep-alice"; p3.owner = owner1;
        BOOST_CHECK(db->WriteProfile("alice", p1));
        BOOST_CHECK(db->WriteProfile("bob", p2));
        BOOST_CHECK(db->WriteProfile("rep-alice", p3));
        ReddIDReputation r1; r1.reddId = "alice"; r1.overallScore = 1.0;
        ReddIDReputation r2; r2.reddId = "profile-bob"; r2.overallScore = 2.0;
        BOOST_CHECK(db->WriteReputation("alice", r1));
        BOOST_CHECK(db->WriteReputation("profile-bob", r2));
    }
    // Connections.
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"alice", "bob"}, {"alice", "carol"}, {"alice-x", "dave"}, {"bob", "alice"}}) {
        ReddIDConnection c; c.fromReddId = from; c.toReddId = to;
        BOOST_CHECK(db->WriteConnection(from, to, c));
    }
    // Deposit locks.
    BOOST_CHECK(db->WriteDepositLock(COutPoint(idA, 0), MakeDepositLock(COutPoint(idA, 0), 0x11)));
    BOOST_CHECK(db->WriteDepositLock(COutPoint(idA, 1), MakeDepositLock(COutPoint(idA, 1), 0x22)));
    // Auction configs.
    BOOST_CHECK(db->WriteAuctionConfig("namespace", AuctionConfig::NamespaceDefaults()));
    BOOST_CHECK(db->WriteAuctionConfig("userid", AuctionConfig::UserIDDefaults()));
    // Resolutions (each writes a forward and a reverse key).
    BOOST_CHECK(db->WriteResolution("alice", "redd", "u1"));
    BOOST_CHECK(db->WriteResolution("alice", "a-b", "u2"));
    BOOST_CHECK(db->WriteResolution("alice-x", "redd", "u3"));
    BOOST_CHECK(db->WriteResolution("bob", "redd", "u4"));

    // 4 ns + 4 tier + 4 id + 2 auc + 3 bid + 3 profile + 2 rep + 4 conn
    // + 2 dep + 2 aconf + 8 res
    {
        size_t count = 0;
        std::unique_ptr<CDBIterator> it(db->NewIterator());
        for (it->SeekToFirst(); it->Valid(); it->Next()) ++count;
        BOOST_CHECK_EQUAL(count, 38U);
    }

    // Namespaces
    {
        std::vector<std::string> ids;
        BOOST_CHECK(db->ListNamespaces(ids));
        BOOST_CHECK_EQUAL(ids.size(), 4U);
        BOOST_CHECK((ToSet(ids) == StrSet{"redd", "a", "a-b", "tier-a"}));
    }

    // Pricing tiers. The prefix is "tier-<ns>-", so a namespace whose id is a
    // '-'-terminated prefix of another also gets the other's tiers: asking for
    // "a" returns the tier of "a-b" as well (current behaviour).
    {
        std::vector<PricingTier> tiers;
        BOOST_CHECK(db->ReadPricingTiers("redd", tiers));
        BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
        BOOST_CHECK_EQUAL(tiers[0].namespaceId, "redd");
        BOOST_CHECK_EQUAL(tiers[0].minLength, 1);

        tiers.clear();
        BOOST_CHECK(db->ReadPricingTiers("a-b", tiers));
        BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
        BOOST_CHECK_EQUAL(tiers[0].namespaceId, "a-b");

        tiers.clear();
        BOOST_CHECK(db->ReadPricingTiers("a", tiers));
        BOOST_CHECK_EQUAL(tiers.size(), 3U);
        std::multiset<std::string> owners;
        for (const PricingTier& t : tiers) owners.insert(t.namespaceId);
        BOOST_CHECK((owners == std::multiset<std::string>{"a", "a", "a-b"}));

        tiers.clear();
        BOOST_CHECK(db->ReadPricingTiers("tier-a", tiers));
        BOOST_CHECK(tiers.empty());
    }

    // User IDs
    {
        std::vector<std::string> names;
        BOOST_CHECK(db->ListUserIDs("redd", names));
        BOOST_CHECK_EQUAL(names.size(), 2U);
        BOOST_CHECK((ToSet(names) == StrSet{"alice", "bob"}));

        names.clear();
        BOOST_CHECK(db->ListUserIDs("a-b", names));
        BOOST_CHECK((names == std::vector<std::string>{"carol"}));

        names.clear();
        BOOST_CHECK(db->ListUserIDs("a", names));
        BOOST_CHECK((names == std::vector<std::string>{"ns-x"}));

        // An empty namespace id lists every user ID.
        names.clear();
        BOOST_CHECK(db->ListUserIDs("", names));
        BOOST_CHECK_EQUAL(names.size(), 4U);
        BOOST_CHECK((ToSet(names) == StrSet{"alice", "bob", "carol", "ns-x"}));
    }

    // Auctions
    {
        std::vector<uint256> ids;
        BOOST_CHECK(db->ListAuctions(ids));
        BOOST_CHECK_EQUAL(ids.size(), 2U);
        BOOST_CHECK((ToSet(ids) == std::set<uint256>{idA, idB}));

        ids.clear();
        BOOST_CHECK(db->ListAuctionsByState(AUCTION_ACTIVE, ids));
        BOOST_CHECK((ids == std::vector<uint256>{idA}));

        ids.clear();
        BOOST_CHECK(db->ListAuctionsByState(AUCTION_PENDING, ids));
        BOOST_CHECK(ids.empty());
    }

    // Bids
    {
        std::vector<uint256> ids;
        BOOST_CHECK(db->ListBids(idA, ids));
        BOOST_CHECK_EQUAL(ids.size(), 2U);
        BOOST_CHECK((ToSet(ids) == std::set<uint256>{idA, idC}));

        ids.clear();
        BOOST_CHECK(db->ListBids(idB, ids));
        BOOST_CHECK((ids == std::vector<uint256>{idB}));

        ids.clear();
        BOOST_CHECK(db->ListBids(idC, ids));
        BOOST_CHECK(ids.empty());
    }

    // Profiles
    {
        std::vector<std::string> ids;
        BOOST_CHECK(db->ListProfiles(ids));
        BOOST_CHECK_EQUAL(ids.size(), 3U);
        BOOST_CHECK((ToSet(ids) == StrSet{"alice", "bob", "rep-alice"}));

        ids.clear();
        BOOST_CHECK(db->ListProfilesByOwner(owner1, ids));
        BOOST_CHECK_EQUAL(ids.size(), 2U);
        BOOST_CHECK((ToSet(ids) == StrSet{"alice", "rep-alice"}));

        ids.clear();
        BOOST_CHECK(db->ListProfilesByOwner(owner2, ids));
        BOOST_CHECK((ids == std::vector<std::string>{"bob"}));
    }

    // Reputations (no list method; the two records stay distinct from the
    // profiles with similar ids).
    {
        ReddIDReputation rep;
        BOOST_CHECK(db->ReadReputation("alice", rep));
        BOOST_CHECK_EQUAL(rep.overallScore, 1.0);
        BOOST_CHECK(db->ReadReputation("profile-bob", rep));
        BOOST_CHECK_EQUAL(rep.overallScore, 2.0);
        BOOST_CHECK(!db->ReadReputation("bob", rep));
        BOOST_CHECK(!db->ReadReputation("rep-alice", rep));
    }

    // Connections. Same prefix rule as the tiers: "alice" also gets the
    // connections of "alice-x" (current behaviour).
    {
        std::vector<ReddIDConnection> conns;
        BOOST_CHECK(db->ListConnections("bob", conns));
        BOOST_REQUIRE_EQUAL(conns.size(), 1U);
        BOOST_CHECK_EQUAL(conns[0].fromReddId, "bob");
        BOOST_CHECK_EQUAL(conns[0].toReddId, "alice");

        conns.clear();
        BOOST_CHECK(db->ListConnections("alice-x", conns));
        BOOST_REQUIRE_EQUAL(conns.size(), 1U);
        BOOST_CHECK_EQUAL(conns[0].toReddId, "dave");

        conns.clear();
        BOOST_CHECK(db->ListConnections("alice", conns));
        BOOST_CHECK_EQUAL(conns.size(), 3U);
        std::set<std::pair<std::string, std::string>> pairs;
        for (const ReddIDConnection& c : conns) pairs.emplace(c.fromReddId, c.toReddId);
        BOOST_CHECK((pairs == std::set<std::pair<std::string, std::string>>{{"alice", "bob"}, {"alice", "carol"}, {"alice-x", "dave"}}));

        conns.clear();
        BOOST_CHECK(db->ListConnections("carol", conns));
        BOOST_CHECK(conns.empty());
    }

    // Deposit locks
    {
        std::vector<DepositLock> locks;
        BOOST_CHECK(db->ListAllDepositLocks(locks));
        BOOST_CHECK_EQUAL(locks.size(), 2U);
        std::set<COutPoint> utxos;
        for (const DepositLock& l : locks) utxos.insert(l.utxo);
        BOOST_CHECK((utxos == std::set<COutPoint>{COutPoint(idA, 0), COutPoint(idA, 1)}));
    }

    // Auction configs
    {
        std::vector<AuctionConfig> configs;
        BOOST_CHECK(db->ListAuctionConfigs(configs));
        BOOST_CHECK_EQUAL(configs.size(), 2U);
        StrSet ids;
        for (const AuctionConfig& c : configs) ids.insert(c.configId);
        BOOST_CHECK((ids == StrSet{"namespace", "userid"}));
    }

    // Resolutions: only forward entries, never the reverse ones. Same prefix
    // rule again: "alice" also gets the entry of "alice-x", under the map key
    // "x-redd" (current behaviour).
    {
        std::map<std::string, std::string> res;
        BOOST_CHECK(db->ListResolutions("bob", res));
        BOOST_CHECK((res == std::map<std::string, std::string>{{"redd", "u4"}}));

        res.clear();
        BOOST_CHECK(db->ListResolutions("alice-x", res));
        BOOST_CHECK((res == std::map<std::string, std::string>{{"redd", "u3"}}));

        res.clear();
        BOOST_CHECK(db->ListResolutions("alice", res));
        BOOST_CHECK((res == std::map<std::string, std::string>{{"redd", "u1"}, {"a-b", "u2"}, {"x-redd", "u3"}}));

        res.clear();
        BOOST_CHECK(db->ListResolutions("u1", res));
        BOOST_CHECK(res.empty());
        BOOST_CHECK(db->ListResolutions("userid", res));
        BOOST_CHECK(res.empty());
    }
}

BOOST_AUTO_TEST_CASE(composite_keys_are_ambiguous_across_separators)
{
    // Composite keys join their parts with '-' (or '.' for user IDs) without
    // escaping, so different part pairs can map to one key. Current behaviour:
    // the later write replaces the earlier record.
    ReddIDConnection c1; c1.fromReddId = "a-b"; c1.toReddId = "c";
    ReddIDConnection c2; c2.fromReddId = "a"; c2.toReddId = "b-c";
    BOOST_CHECK(db->WriteConnection("a-b", "c", c1));
    BOOST_CHECK(db->WriteConnection("a", "b-c", c2));
    ReddIDConnection connOut;
    BOOST_CHECK(db->ReadConnection("a-b", "c", connOut));
    BOOST_CHECK_EQUAL(connOut.fromReddId, "a");
    BOOST_CHECK_EQUAL(connOut.toReddId, "b-c");

    UserIDInfo u1; u1.name = "x.y"; u1.namespaceId = "z";
    UserIDInfo u2; u2.name = "x"; u2.namespaceId = "y.z";
    BOOST_CHECK(db->WriteUserID("x.y", "z", u1));
    BOOST_CHECK(db->WriteUserID("x", "y.z", u2));
    UserIDInfo uidOut;
    BOOST_CHECK(db->ReadUserID("x.y", "z", uidOut));
    BOOST_CHECK_EQUAL(uidOut.name, "x");
    BOOST_CHECK_EQUAL(uidOut.namespaceId, "y.z");
    // ListUserIDs splits the key at the last '.', whatever the record says.
    std::vector<std::string> names;
    BOOST_CHECK(db->ListUserIDs("z", names));
    BOOST_CHECK((names == std::vector<std::string>{"x.y"}));
    names.clear();
    BOOST_CHECK(db->ListUserIDs("y.z", names));
    BOOST_CHECK(names.empty());

    PricingTier t1; t1.namespaceId = "n-"; t1.minLength = 1; t1.minPrice = 1;
    PricingTier t2; t2.namespaceId = "n"; t2.minLength = -1; t2.minPrice = 2;
    BOOST_CHECK(db->WritePricingTier(t1)); // tier-n--1
    BOOST_CHECK(db->WritePricingTier(t2)); // tier-n--1
    std::vector<PricingTier> tiers;
    BOOST_CHECK(db->ReadPricingTiers("n", tiers));
    BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
    BOOST_CHECK_EQUAL(tiers[0].namespaceId, "n");
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 2);

    BOOST_CHECK(db->WriteResolution("r-s", "t", "first"));
    BOOST_CHECK(db->WriteResolution("r", "s-t", "second"));
    std::string userId;
    BOOST_CHECK(db->ReadResolution("r-s", "t", userId));
    BOOST_CHECK_EQUAL(userId, "second");
}

BOOST_AUTO_TEST_SUITE_END()
