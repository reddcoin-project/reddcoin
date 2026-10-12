// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key.h>
#include <primitives/block.h>
#include <test/reddid_auction_test_util.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>

#include <cassert>
#include <string>
#include <vector>

using namespace reddid_test;

/**
 * AuctionTestingSetup with shorthands for user IDs, which are kept in the
 * database alone.
 */
struct UserIDTestingSetup : public AuctionTestingSetup {
    NamespaceManager* nsMgr;

    UserIDTestingSetup() : nsMgr(reddidManager->GetNamespaceManager()) {}

    /** Register a user ID in the database, as the end of an auction does. */
    UserIDInfo AddUserID(const std::string& name, const std::string& namespaceId, const CKeyID& owner,
                         int64_t expirationTime, int transactionCount = 0)
    {
        UserIDInfo info;
        info.name = name;
        info.namespaceId = namespaceId;
        info.owner = owner;
        info.registrationTime = T0 - 10 * DAY;
        info.expirationTime = expirationTime;
        info.lastTransaction = T0 - DAY;
        info.transactionCount = transactionCount;
        info.metadataHash = uint256S("3e7a");
        bool written = db->WriteUserID(name, namespaceId, info);
        assert(written);
        return info;
    }

    bool Exists(const std::string& name, const std::string& namespaceId) { return db->ExistsUserID(name, namespaceId); }

    UserIDInfo Stored(const std::string& name, const std::string& namespaceId)
    {
        UserIDInfo info;
        bool found = db->ReadUserID(name, namespaceId, info);
        assert(found);
        return info;
    }

    /** A namespace like the one of the fixture, with the given grace period in days. */
    void AddNamespace(const std::string& namespaceId, int gracePeriod)
    {
        NamespaceInfo info;
        bool found = nsMgr->GetNamespaceInfo(NS, info);
        assert(found);
        info.id = namespaceId;
        info.gracePeriod = gracePeriod;
        bool created = nsMgr->CreateNamespace(info);
        assert(created);
    }
};

/**
 * The user ID operations of ReddIDManager: the auction pass-throughs,
 * GetUserIDInfo, TransferUserID, RenewUserID, CalculateRenewalFee,
 * ExpireUserID and CheckExpiredUserIDs.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_userid_tests, UserIDTestingSetup)

// ===================================================================
// Category 1: Auctions for user IDs
// ===================================================================

BOOST_AUTO_TEST_CASE(create_user_id_auction_creates_an_auction)
{
    uint256 auctionId;
    BOOST_CHECK(reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(auctionId == ExpectedAuctionId("alice", NS, T0));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.name, "alice");
    BOOST_CHECK_EQUAL(info.namespaceId, NS);
    BOOST_CHECK(info.creator == creator);
    BOOST_CHECK_EQUAL(info.reservePrice, 5000 * COIN);
    BOOST_CHECK_EQUAL(info.startTime, T0);
    BOOST_CHECK_EQUAL(info.endTime, T0 + 5 * DAY);
    BOOST_CHECK_EQUAL(info.type, AUCTION_STANDARD);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    AuctionInfo stored;
    BOOST_CHECK(db->ReadAuction(auctionId, stored));

    // The limits of the namespace: 3 to 7 days
    uint256 shortest, longest;
    BOOST_CHECK(reddidManager->CreateUserIDAuction("bob", NS, creator, 5000 * COIN, 3, AUCTION_STANDARD, shortest));
    BOOST_CHECK(reddidManager->CreateUserIDAuction("carol", NS, creator, 5000 * COIN, 7, AUCTION_STANDARD, longest));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, shortest).endTime, T0 + 3 * DAY);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, longest).endTime, T0 + 7 * DAY);
}

BOOST_AUTO_TEST_CASE(create_user_id_auction_rejected)
{
    uint256 auctionId;
    // The name
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("ab", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("Alice", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("-alice", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    // The namespace
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", "nosuchns", creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", "", creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
    // The duration
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, 2, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, 8, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, 0, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, -5, AUCTION_STANDARD, auctionId));
    // The reserve price
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN - 1, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 0, 5, AUCTION_STANDARD, auctionId));

    BOOST_CHECK(auctionId.IsNull());
    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK(auctionIds.empty());

    // The minimum is the price of the pricing tier for the length of the name
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 9000 * COIN));
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("alice", NS, creator, 9000 * COIN - 1, 5, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(reddidManager->CreateUserIDAuction("alice", NS, creator, 9000 * COIN, 5, AUCTION_STANDARD, auctionId));
}

BOOST_AUTO_TEST_CASE(user_id_operations_on_a_stopped_manager)
{
    uint256 auctionId = NewAuction("alice");
    BOOST_REQUIRE(reddidManager->Stop());

    uint256 newId;
    BOOST_CHECK(!reddidManager->CreateUserIDAuction("bob", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, newId));
    BOOST_CHECK(newId.IsNull());
    BOOST_CHECK(!reddidManager->CancelUserIDAuction(auctionId, creator));
    // ExpireUserID and CheckExpiredUserIDs check that there is a database
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!reddidManager->ExpireUserID("alice", NS));
    // Current behaviour: the other user ID operations (FinalizeUserIDAuction,
    // TransferUserID, RenewUserID, GetUserIDInfo, GetAuctionBids and
    // CalculateRenewalFee) check nothing. On a manager that is not
    // initialized they call through null pointers, so they are not called here.
}

BOOST_AUTO_TEST_CASE(cancel_user_id_auction_is_for_the_creator)
{
    uint256 auctionId = NewAuction("alice");

    BOOST_CHECK(!reddidManager->CancelUserIDAuction(auctionId, MakeKeyID()));
    BOOST_CHECK(!reddidManager->CancelUserIDAuction(auctionId, CKeyID()));
    BOOST_CHECK(!reddidManager->CancelUserIDAuction(uint256S("01"), creator));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    BOOST_CHECK(reddidManager->CancelUserIDAuction(auctionId, creator));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_CANCELED);
}

BOOST_AUTO_TEST_CASE(finalize_user_id_auction_registers_the_name)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, bidId));

    BOOST_CHECK(!reddidManager->FinalizeUserIDAuction(uint256S("01")));
    // Not before the end
    BOOST_CHECK(!reddidManager->FinalizeUserIDAuction(auctionId));
    UserIDInfo userId;
    BOOST_CHECK(!reddidManager->GetUserIDInfo("alice", NS, userId));

    EndAuction(auctionId);
    BOOST_CHECK(reddidManager->FinalizeUserIDAuction(auctionId));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_FINALIZED);
    BOOST_REQUIRE(reddidManager->GetUserIDInfo("alice", NS, userId));
    BOOST_CHECK_EQUAL(userId.name, "alice");
    BOOST_CHECK_EQUAL(userId.namespaceId, NS);
    BOOST_CHECK(userId.owner == bidder);
    BOOST_CHECK_EQUAL(userId.registrationTime, T0 + 7 * DAY);
    BOOST_CHECK_EQUAL(userId.expirationTime, T0 + 7 * DAY + 365 * DAY);

    BOOST_CHECK(!reddidManager->FinalizeUserIDAuction(auctionId));
}

BOOST_AUTO_TEST_CASE(get_auction_bids_forwards_to_the_auction_manager)
{
    uint256 auctionId = NewAuction("alice");
    BOOST_CHECK(reddidManager->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(reddidManager->GetAuctionBids(uint256S("01")).empty());

    CKeyID first = MakeKeyID();
    CKeyID second = MakeKeyID();
    uint256 firstBid, secondBid;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, first, 6000 * COIN, firstBid));
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, second, 7000 * COIN, secondBid));

    std::vector<BidInfo> bids = reddidManager->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    BOOST_CHECK(bids[0].bidId == firstBid);
    BOOST_CHECK(bids[0].bidder == first);
    BOOST_CHECK(bids[1].bidId == secondBid);
    BOOST_CHECK_EQUAL(bids[1].bidAmount, 7000 * COIN);
}

// ===================================================================
// Category 2: GetUserIDInfo, TransferUserID, RenewUserID
// ===================================================================

BOOST_AUTO_TEST_CASE(get_user_id_info_reads_the_database)
{
    UserIDInfo info;
    BOOST_CHECK(!reddidManager->GetUserIDInfo("alice", NS, info));

    CKeyID owner = MakeKeyID();
    AddUserID("alice", NS, owner, T0 + 100 * DAY, 12);

    BOOST_REQUIRE(reddidManager->GetUserIDInfo("alice", NS, info));
    BOOST_CHECK_EQUAL(info.name, "alice");
    BOOST_CHECK_EQUAL(info.namespaceId, NS);
    BOOST_CHECK(info.owner == owner);
    BOOST_CHECK_EQUAL(info.registrationTime, T0 - 10 * DAY);
    BOOST_CHECK_EQUAL(info.expirationTime, T0 + 100 * DAY);
    BOOST_CHECK_EQUAL(info.lastTransaction, T0 - DAY);
    BOOST_CHECK_EQUAL(info.transactionCount, 12);
    BOOST_CHECK(info.metadataHash == uint256S("3e7a"));

    // Name and namespace are both part of the key
    BOOST_CHECK(!reddidManager->GetUserIDInfo("alice", "otherns", info));
    BOOST_CHECK(!reddidManager->GetUserIDInfo("alic", NS, info));
    // Neither needs to be valid, nor does the namespace need to exist
    AddUserID("Not Valid", "nosuchns", owner, 0);
    BOOST_CHECK(reddidManager->GetUserIDInfo("Not Valid", "nosuchns", info));
}

// Current behaviour: a transfer is a write to the database of this node. No
// transaction carries it, so no other node learns of it and no block can
// undo it. The only check is that the caller names the current owner.
BOOST_AUTO_TEST_CASE(transfer_user_id_changes_the_owner)
{
    CKeyID owner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    UserIDInfo before = AddUserID("alice", NS, owner, T0 + 100 * DAY, 12);

    BOOST_CHECK(!reddidManager->TransferUserID("alice", NS, newOwner, newOwner));
    BOOST_CHECK(!reddidManager->TransferUserID("alice", NS, CKeyID(), newOwner));
    BOOST_CHECK(!reddidManager->TransferUserID("bob", NS, owner, newOwner));
    BOOST_CHECK(!reddidManager->TransferUserID("alice", "otherns", owner, newOwner));
    BOOST_CHECK(Stored("alice", NS).owner == owner);

    BOOST_CHECK(reddidManager->TransferUserID("alice", NS, owner, newOwner));

    UserIDInfo after = Stored("alice", NS);
    BOOST_CHECK(after.owner == newOwner);
    BOOST_CHECK_EQUAL(after.registrationTime, before.registrationTime);
    BOOST_CHECK_EQUAL(after.expirationTime, before.expirationTime);
    BOOST_CHECK_EQUAL(after.lastTransaction, before.lastTransaction);
    BOOST_CHECK_EQUAL(after.transactionCount, before.transactionCount);
    BOOST_CHECK(after.metadataHash == before.metadataHash);

    // The previous owner has no say any more
    BOOST_CHECK(!reddidManager->TransferUserID("alice", NS, owner, owner));
    // An owner can transfer to itself
    BOOST_CHECK(reddidManager->TransferUserID("alice", NS, newOwner, newOwner));
}

// Current behaviour: nothing but the owner is checked. An expired user ID,
// past its grace period, can still be transferred, and a user ID can be
// transferred to the null key, which nobody can transfer it away from by
// accident: the null key is then the owner that the next caller must name.
BOOST_AUTO_TEST_CASE(transfer_user_id_checks_nothing_but_the_owner)
{
    CKeyID owner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    AddUserID("old", NS, owner, T0 - 400 * DAY);
    BOOST_CHECK(reddidManager->TransferUserID("old", NS, owner, newOwner));
    BOOST_CHECK(Stored("old", NS).owner == newOwner);

    AddUserID("alice", NS, owner, T0 + 100 * DAY);
    BOOST_CHECK(reddidManager->TransferUserID("alice", NS, owner, CKeyID()));
    BOOST_CHECK(Stored("alice", NS).owner.IsNull());
    BOOST_CHECK(!reddidManager->TransferUserID("alice", NS, owner, owner));
    BOOST_CHECK(reddidManager->TransferUserID("alice", NS, CKeyID(), owner));

    // The namespace does not need to exist
    AddUserID("orphan", "nosuchns", owner, T0 + 100 * DAY);
    BOOST_CHECK(reddidManager->TransferUserID("orphan", "nosuchns", owner, newOwner));
}

BOOST_AUTO_TEST_CASE(renew_user_id_extends_the_expiration)
{
    CKeyID owner = MakeKeyID();
    UserIDInfo before = AddUserID("alice", NS, owner, T0 + 100 * DAY, 12);

    BOOST_CHECK(!reddidManager->RenewUserID("alice", NS, MakeKeyID()));
    BOOST_CHECK(!reddidManager->RenewUserID("bob", NS, owner));
    BOOST_CHECK_EQUAL(Stored("alice", NS).expirationTime, T0 + 100 * DAY);

    // Not expired: the renewal period of the namespace, 365 days, is added
    BOOST_CHECK(reddidManager->RenewUserID("alice", NS, owner));
    UserIDInfo after = Stored("alice", NS);
    BOOST_CHECK_EQUAL(after.expirationTime, T0 + 465 * DAY);
    BOOST_CHECK(after.owner == owner);
    BOOST_CHECK_EQUAL(after.registrationTime, before.registrationTime);
    BOOST_CHECK_EQUAL(after.transactionCount, 12);

    // And again: renewals add up
    BOOST_CHECK(reddidManager->RenewUserID("alice", NS, owner));
    BOOST_CHECK_EQUAL(Stored("alice", NS).expirationTime, T0 + 830 * DAY);

    // The period comes from the namespace
    NamespaceInfo shortPeriod;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo(NS, shortPeriod));
    shortPeriod.id = "shortns";
    shortPeriod.renewalPeriod = 90;
    BOOST_REQUIRE(nsMgr->CreateNamespace(shortPeriod));
    AddUserID("carol", "shortns", owner, T0 + 100 * DAY);
    BOOST_CHECK(reddidManager->RenewUserID("carol", "shortns", owner));
    BOOST_CHECK_EQUAL(Stored("carol", "shortns").expirationTime, T0 + 190 * DAY);
}

BOOST_AUTO_TEST_CASE(renew_expired_user_id_counts_from_now)
{
    CKeyID owner = MakeKeyID();

    // Expiring this second counts as expired
    AddUserID("now", NS, owner, T0);
    BOOST_CHECK(reddidManager->RenewUserID("now", NS, owner));
    BOOST_CHECK_EQUAL(Stored("now", NS).expirationTime, T0 + 365 * DAY);

    AddUserID("next", NS, owner, T0 + 1);
    BOOST_CHECK(reddidManager->RenewUserID("next", NS, owner));
    BOOST_CHECK_EQUAL(Stored("next", NS).expirationTime, T0 + 1 + 365 * DAY);

    // Current behaviour: a user ID far past its grace period, which the
    // periodic task would erase the next time it runs, can still be renewed
    AddUserID("old", NS, owner, T0 - 400 * DAY);
    BOOST_CHECK(reddidManager->RenewUserID("old", NS, owner));
    BOOST_CHECK_EQUAL(Stored("old", NS).expirationTime, T0 + 365 * DAY);

    // A user ID that never expires (expiration 0) counts as expired too and gets an expiration
    AddUserID("forever", NS, owner, 0);
    BOOST_CHECK(reddidManager->RenewUserID("forever", NS, owner));
    BOOST_CHECK_EQUAL(Stored("forever", NS).expirationTime, T0 + 365 * DAY);
}

// Current behaviour: like a transfer, a renewal is a write to the database of
// this node, and it is free: CalculateRenewalFee is not consulted and nothing
// is paid.
BOOST_AUTO_TEST_CASE(renew_user_id_needs_the_namespace_and_no_fee)
{
    CKeyID owner = MakeKeyID();
    AddUserID("orphan", "nosuchns", owner, T0 + 100 * DAY);
    BOOST_CHECK(!reddidManager->RenewUserID("orphan", "nosuchns", owner));
    BOOST_CHECK_EQUAL(Stored("orphan", "nosuchns").expirationTime, T0 + 100 * DAY);

    AddUserID("alice", NS, owner, T0 + 100 * DAY);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 2500 * COIN);
    for (int i = 1; i <= 10; i++) {
        BOOST_CHECK(reddidManager->RenewUserID("alice", NS, owner));
    }
    BOOST_CHECK_EQUAL(Stored("alice", NS).expirationTime, T0 + 100 * DAY + 10 * 365 * DAY);
}

// ===================================================================
// Category 3: CalculateRenewalFee
// ===================================================================

BOOST_AUTO_TEST_CASE(renewal_fee_is_half_the_minimum_auction_price)
{
    CKeyID owner = MakeKeyID();
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 0);

    AddUserID("alice", NS, owner, T0 + 100 * DAY);
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID("alice", NS), 5000 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 2500 * COIN);

    // The pricing tiers of the namespace apply
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 9000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 6, 2000 * COIN));
    AddUserID("abcdef", NS, owner, T0 + 100 * DAY);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 4500 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("abcdef", NS), 1000 * COIN);

    // No fee for a user ID that does not exist or whose namespace does not
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("nobody", NS), 0);
    AddUserID("orphan", "nosuchns", owner, T0 + 100 * DAY);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("orphan", "nosuchns"), 0);

    // The fee does not depend on the clock or on whether the user ID has expired
    SetMockTime(T0 + 1000 * DAY);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 4500 * COIN);
}

BOOST_AUTO_TEST_CASE(renewal_fee_usage_discount)
{
    CKeyID owner = MakeKeyID();
    const CAmount full = 2500 * COIN;

    // 5% for every ten transactions, counted in whole tens, up to 25%
    struct { int transactions; int percent; } cases[] = {
        {0, 0}, {1, 0}, {9, 0}, {10, 5}, {19, 5}, {20, 10}, {30, 15}, {40, 20}, {49, 20}, {50, 25}, {51, 25}, {1000, 25},
    };
    for (const auto& c : cases) {
        AddUserID("alice", NS, owner, T0 + 100 * DAY, c.transactions);
        BOOST_CHECK_MESSAGE(reddidManager->CalculateRenewalFee("alice", NS) == full * (100 - c.percent) / 100,
                            "transactions " << c.transactions << ": fee " << reddidManager->CalculateRenewalFee("alice", NS));
    }

    // A negative count gives no discount
    AddUserID("alice", NS, owner, T0 + 100 * DAY, -100);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), full);

    // The fee is rounded down: half of an odd price loses the half base unit
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 9001));
    AddUserID("alice", NS, owner, T0 + 100 * DAY, 0);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 4500);
    AddUserID("alice", NS, owner, T0 + 100 * DAY, 10);
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 4275);
    // and so does the fee less the discount: 95% of 4501 is 4275.95
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 9003));
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), 4275);
    // The largest price there can be
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, MAX_MONEY));
    BOOST_CHECK_EQUAL(reddidManager->CalculateRenewalFee("alice", NS), MAX_MONEY / 2 / 100 * 95);
}

// ===================================================================
// Category 4: ExpireUserID and CheckExpiredUserIDs
// ===================================================================

BOOST_AUTO_TEST_CASE(expire_user_id_erases_the_record)
{
    CKeyID owner = MakeKeyID();
    // ExpireUserID does not look at the expiration time
    AddUserID("alice", NS, owner, T0 + 100 * DAY);
    AddUserID("bob", NS, owner, T0 + 100 * DAY);

    BOOST_CHECK(reddidManager->ExpireUserID("alice", NS));

    BOOST_CHECK(!Exists("alice", NS));
    BOOST_CHECK(Exists("bob", NS));
    std::vector<std::string> names;
    BOOST_REQUIRE(db->ListUserIDs(NS, names));
    BOOST_REQUIRE_EQUAL(names.size(), 1U);
    BOOST_CHECK_EQUAL(names[0], "bob");

    // Erasing a user ID that does not exist succeeds as well
    BOOST_CHECK(reddidManager->ExpireUserID("alice", NS));
    BOOST_CHECK(reddidManager->ExpireUserID("nobody", "nosuchns"));
}

BOOST_AUTO_TEST_CASE(check_expired_user_ids_waits_for_the_grace_period)
{
    CKeyID owner = MakeKeyID();
    // The namespace of the fixture has a grace period of 30 days
    AddUserID("alice", NS, owner, T0 + 10 * DAY);
    AddUserID("bob", NS, owner, T0 + 10 * DAY + 1);
    AddUserID("forever", NS, owner, 0);

    reddidManager->CheckExpiredUserIDs();
    SetMockTime(T0 + 10 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    // Exactly at the end of the grace period is not yet past it
    SetMockTime(T0 + 40 * DAY);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(Exists("alice", NS));
    BOOST_CHECK(Exists("bob", NS));

    SetMockTime(T0 + 40 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("alice", NS));
    BOOST_CHECK(Exists("bob", NS));

    SetMockTime(T0 + 40 * DAY + 2);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("bob", NS));

    // An expiration time of 0 means that the user ID does not expire
    SetMockTime(T0 + 100000 * DAY);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(Exists("forever", NS));
}

BOOST_AUTO_TEST_CASE(grace_period_comes_from_the_namespace)
{
    CKeyID owner = MakeKeyID();
    // A namespace accepts grace periods of 14 to 90 days
    AddNamespace("shortgrace", 14);
    AddNamespace("longgrace", 90);
    AddUserID("alice", "shortgrace", owner, T0 + 10 * DAY);
    AddUserID("alice", "longgrace", owner, T0 + 10 * DAY);
    AddUserID("alice", NS, owner, T0 + 10 * DAY);

    SetMockTime(T0 + 24 * DAY);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(Exists("alice", "shortgrace"));

    SetMockTime(T0 + 24 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("alice", "shortgrace"));
    BOOST_CHECK(Exists("alice", NS));
    BOOST_CHECK(Exists("alice", "longgrace"));

    SetMockTime(T0 + 100 * DAY);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("alice", NS));
    BOOST_CHECK(Exists("alice", "longgrace"));

    SetMockTime(T0 + 100 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("alice", "longgrace"));
}

// Current behaviour: CheckExpiredUserIDs goes through the namespaces in the
// database and the user IDs of each. A user ID whose namespace has no record
// is never looked at and so never expires.
BOOST_AUTO_TEST_CASE(user_id_of_a_namespace_without_a_record_never_expires)
{
    CKeyID owner = MakeKeyID();
    AddUserID("orphan", "nosuchns", owner, T0 - 400 * DAY);
    AddUserID("alice", NS, owner, T0 - 400 * DAY);

    reddidManager->CheckExpiredUserIDs();

    BOOST_CHECK(!Exists("alice", NS));
    BOOST_CHECK(Exists("orphan", "nosuchns"));
}

// SUSPECTED BUG (documented, not fixed): user IDs expire by the clock of the
// node at the moment the periodic task runs, not by anything in the chain,
// and the record is erased from the database with nothing kept to restore
// it. Two nodes on the same chain whose clocks differ, or of which one was
// not running, disagree on whether a user ID exists; neither turning the
// clock back nor disconnecting blocks brings an erased user ID back, and the
// name can be auctioned again at once.
BOOST_AUTO_TEST_CASE(expiry_follows_the_clock_and_cannot_be_undone)
{
    CKeyID owner = MakeKeyID();
    AddUserID("alice", NS, owner, T0 + 10 * DAY);
    AddUserID("bob", NS, owner, T0 + 10 * DAY + 1);

    // A node whose clock is one second ahead erases the user ID...
    SetMockTime(T0 + 40 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(!Exists("alice", NS));

    // ...and when its clock is corrected the user ID is still gone
    SetMockTime(T0 + 40 * DAY);
    reddidManager->CheckExpiredUserIDs();
    UserIDInfo info;
    BOOST_CHECK(!reddidManager->GetUserIDInfo("alice", NS, info));
    BOOST_CHECK(!reddidManager->RenewUserID("alice", NS, owner));

    // Undoing a block does not look at user IDs that expired
    BOOST_CHECK(reddidManager->UndoBlock(CBlock(), 1));
    BOOST_CHECK(!Exists("alice", NS));

    // The other one is in the last second of its grace period: renewed now it is kept
    BOOST_CHECK(reddidManager->RenewUserID("bob", NS, owner));
    SetMockTime(T0 + 40 * DAY + 2);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(Exists("bob", NS));

    // The erased name is free for a new auction
    uint256 auctionId;
    BOOST_CHECK(reddidManager->CreateUserIDAuction("alice", NS, creator, 5000 * COIN, 5, AUCTION_STANDARD, auctionId));
}

// Expiry of the namespace takes its user IDs with it, whatever their own
// expiration; CheckExpiredUserIDs is not needed for that.
BOOST_AUTO_TEST_CASE(user_ids_go_with_their_namespace)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo expiring;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo(NS, expiring));
    expiring.id = "expns";
    expiring.expiration = T0 + 10 * DAY;
    BOOST_REQUIRE(nsMgr->CreateNamespace(expiring));
    AddUserID("alice", "expns", owner, T0 + 3650 * DAY);
    AddUserID("forever", "expns", owner, 0);
    AddUserID("alice", NS, owner, T0 + 3650 * DAY);

    SetMockTime(T0 + 40 * DAY + 1);
    reddidManager->CheckExpiredUserIDs();
    BOOST_CHECK(Exists("alice", "expns"));

    nsMgr->CheckExpiredNamespaces();
    BOOST_CHECK(!Exists("alice", "expns"));
    BOOST_CHECK(!Exists("forever", "expns"));
    BOOST_CHECK(Exists("alice", NS));
}

BOOST_AUTO_TEST_SUITE_END()
