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
#include <test/reddid_auction_test_util.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <memory>
#include <string>
#include <vector>

using namespace reddid_test;

namespace {
/**
 * Puts the ReddID database of the fixture on disk for the lifetime of the
 * object, so that it outlives ReddIDManager::Stop() and is found again by
 * Init(). The fixture's own database is in memory and is lost on Stop().
 */
struct DatabaseOnDisk {
    DatabaseOnDisk() { gArgs.ForceSetArg("-reddidbmemory", "0"); }
    ~DatabaseOnDisk() { gArgs.ForceSetArg("-reddidbmemory", "1"); }
};

/** Stop the manager and bring it up again, as a node does when it is restarted. */
void Restart(ReddIDManager& manager)
{
    BOOST_REQUIRE(manager.Stop());
    BOOST_REQUIRE(manager.Init());
    BOOST_REQUIRE(manager.Start());
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_manager_tests, ReddIDTestingSetup)

// ===================================================================
// Category 1: Lifecycle
// ===================================================================

BOOST_AUTO_TEST_CASE(fixture_manager_is_initialized_and_running)
{
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->GetReddIDDB() != nullptr);

    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    BOOST_REQUIRE(nsMgr != nullptr);
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(nsMgr->IsRunning());
    BOOST_CHECK(nsMgr->GetDB() == reddidManager->GetReddIDDB());

    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(aMgr != nullptr);
    BOOST_CHECK(aMgr->IsInitialized());
    BOOST_CHECK(aMgr->IsRunning());
    BOOST_CHECK(aMgr->GetDB() == reddidManager->GetReddIDDB());
    BOOST_CHECK(aMgr->GetNamespaceManager() == nsMgr);
}

BOOST_AUTO_TEST_CASE(manager_uses_active_chainstate)
{
    BOOST_REQUIRE(m_node.chainman);
    BOOST_CHECK(reddidManager->GetChainState() == &m_node.chainman->ActiveChainstate());
}

BOOST_AUTO_TEST_CASE(no_p2p_manager_yet)
{
    BOOST_CHECK(reddidManager->GetP2PManager() == nullptr);
    BOOST_CHECK(reddidManager->GetNamespaceManager()->GetP2P() == nullptr);
    BOOST_CHECK(reddidManager->GetAuctionManager()->GetP2P() == nullptr);
}

BOOST_AUTO_TEST_CASE(init_and_start_are_idempotent)
{
    ReddIDDB* db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    AuctionManager* aMgr = reddidManager->GetAuctionManager();

    BOOST_CHECK(reddidManager->Init());
    BOOST_CHECK(reddidManager->Start());

    // Nothing was re-created
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == aMgr);
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->IsRunning());
}

BOOST_AUTO_TEST_CASE(start_before_init_fails)
{
    ReddIDManager manager(m_node);
    BOOST_CHECK(!manager.IsInitialized());
    BOOST_CHECK(!manager.Start());
    BOOST_CHECK(!manager.IsRunning());
    BOOST_CHECK(manager.GetReddIDDB() == nullptr);
    BOOST_CHECK(manager.GetNamespaceManager() == nullptr);
    BOOST_CHECK(manager.GetAuctionManager() == nullptr);
    BOOST_CHECK(manager.GetChainState() == nullptr);

    // Interrupt and Stop on a manager that never started are no-ops
    manager.Interrupt();
    BOOST_CHECK(manager.Stop());
}

BOOST_AUTO_TEST_CASE(disabled_manager_inits_but_does_not_start)
{
    // ReddID is off unless -reddid=1 is given; the fixture forces it on
    BOOST_CHECK(!DEFAULT_ENABLE_REDDID);
    gArgs.ForceSetArg("-reddid", "0");

    // Init() reports success when ReddID is disabled, but creates nothing and
    // leaves the manager uninitialized, so Start() then fails.
    ReddIDManager manager(m_node);
    BOOST_CHECK(manager.Init());
    BOOST_CHECK(!manager.IsInitialized());
    BOOST_CHECK(manager.GetReddIDDB() == nullptr);
    BOOST_CHECK(manager.GetNamespaceManager() == nullptr);
    BOOST_CHECK(manager.GetAuctionManager() == nullptr);
    BOOST_CHECK(!manager.Start());
    BOOST_CHECK(!manager.IsRunning());

    gArgs.ForceSetArg("-reddid", "1");
}

BOOST_AUTO_TEST_CASE(stop_releases_components)
{
    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(!reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nullptr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == nullptr);
    BOOST_CHECK(reddidManager->GetReddIDDB() == nullptr);

    // A second Stop() is a no-op
    BOOST_CHECK(reddidManager->Stop());
}

BOOST_AUTO_TEST_CASE(stopped_manager_can_be_initialized_again)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("restart", owner);

    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(reddidManager->Init());
    BOOST_CHECK(reddidManager->Start());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->IsRunning());

    // The fixture's database is in memory, so the new one starts out empty
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    BOOST_REQUIRE(nsMgr != nullptr);
    BOOST_CHECK(nsMgr->GetNamespaces().empty());
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("restart"));

    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(aMgr != nullptr);
    BOOST_CHECK(aMgr->IsInitialized());
    BOOST_CHECK(aMgr->IsRunning());
    BOOST_CHECK(aMgr->GetDB() == reddidManager->GetReddIDDB());
    BOOST_CHECK(aMgr->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());
}

// SUSPECTED BUG (documented, not fixed): Interrupt() clears m_running and
// Stop() returns early when m_running is false. A node shuts down with
// Interrupt() followed by Stop(), so Stop() does none of its work in that
// sequence: the namespace and auction managers are not stopped (and so not
// saved) and the components are not released until the manager is destroyed.
BOOST_AUTO_TEST_CASE(stop_after_interrupt_does_nothing)
{
    ReddIDDB* db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    AuctionManager* aMgr = reddidManager->GetAuctionManager();

    reddidManager->Interrupt();
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(!nsMgr->IsRunning());
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(!aMgr->IsRunning());
    BOOST_CHECK(aMgr->IsInitialized());

    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == aMgr);
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(nsMgr->GetDB() == db);
    BOOST_CHECK(aMgr->IsInitialized());
    BOOST_CHECK(aMgr->GetDB() == db);
}

BOOST_AUTO_TEST_CASE(interrupted_manager_rejects_new_namespace_auctions)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("halted");

    reddidManager->Interrupt();

    uint256 auctionId;
    BOOST_CHECK(!nsMgr->CreateNamespaceAuction("halted", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(auctionId.IsNull());
}

// An interrupted manager is still initialized, so Start() resumes it
BOOST_AUTO_TEST_CASE(interrupted_manager_can_be_started_again)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();

    reddidManager->Interrupt();
    BOOST_CHECK(reddidManager->Start());
    BOOST_CHECK(reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(nsMgr->IsRunning());
    BOOST_CHECK(reddidManager->GetAuctionManager()->IsRunning());
}

// Current behaviour: unlike the namespace manager, the auction manager does
// not look at whether it is running. After Interrupt() it still creates
// auctions and takes bids.
BOOST_AUTO_TEST_CASE(interrupted_auction_manager_still_takes_auctions_and_bids)
{
    MockTimeGuard mockTime(T0);
    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    CKeyID creator = MakeKeyID();
    CreateTestNamespace("halted", creator);

    reddidManager->Interrupt();
    BOOST_CHECK(!aMgr->IsRunning());

    AuctionInfo auction = UserAuction("alice", "halted", creator, 5000 * COIN, T0);
    uint256 auctionId;
    BOOST_CHECK(aMgr->CreateAuction(auction, auctionId));
    uint256 bidId;
    BOOST_CHECK(aMgr->PlaceBid(auctionId, MakeKeyID(), 5000 * COIN, bidId));
    BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
}

BOOST_AUTO_TEST_CASE(auction_manager_is_given_the_active_chainstate)
{
    MockTimeGuard mockTime(T0);
    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    CKeyID creator = MakeKeyID();
    CreateTestNamespace("heights", creator);

    // The auction records the height of the tip of the chain state it was given
    FakeChainHeight tip(m_node.chainman->ActiveChainstate(), 7);
    AuctionInfo auction = UserAuction("alice", "heights", creator, 5000 * COIN, T0);
    uint256 auctionId;
    BOOST_REQUIRE(aMgr->CreateAuction(auction, auctionId));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).blockHeight, 7);
}

// ===================================================================
// Category 2: Namespace pass-throughs
// ===================================================================

BOOST_AUTO_TEST_CASE(validate_namespace_forwards_to_namespace_manager)
{
    BOOST_CHECK(reddidManager->ValidateNamespace("redd"));
    BOOST_CHECK(reddidManager->ValidateNamespace("my-ns"));
    BOOST_CHECK(!reddidManager->ValidateNamespace(""));
    BOOST_CHECK(!reddidManager->ValidateNamespace("REDD"));
    BOOST_CHECK(!reddidManager->ValidateNamespace("-redd"));
    BOOST_CHECK(!reddidManager->ValidateNamespace(std::string(MAX_NAMESPACE_LENGTH + 1, 'x')));
}

BOOST_AUTO_TEST_CASE(min_price_for_namespace_forwards_to_namespace_manager)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    for (const std::string ns : {"a", "ab", "abc", "abcd", "abcdefgh"}) {
        BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForNamespace(ns), nsMgr->CalculateMinPrice(ns));
    }
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForNamespace("a"), 100000 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForNamespace("abcd"), 10000 * COIN);
}

BOOST_AUTO_TEST_CASE(min_price_for_user_id_forwards_to_namespace_manager)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CreateTestNamespace("priced", MakeKeyID());
    BOOST_REQUIRE(nsMgr->AddPricingTier("priced", 3, 9000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier("priced", 6, 2000 * COIN));

    for (const std::string name : {"a", "abc", "abcde", "abcdef", "abcdefghij"}) {
        BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID(name, "priced"),
                          nsMgr->CalculateMinPriceForUserID(name, "priced"));
    }
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID("abcde", "priced"), 9000 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID("abcdef", "priced"), 2000 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID("abcde", "nosuchns"), REDDID_MIN_FEE);
}

// ===================================================================
// Category 3: Auctions and the two managers that keep them
// ===================================================================

BOOST_AUTO_TEST_CASE(auction_state_survives_a_restart_with_a_database_on_disk)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*reddidManager);

    CKeyID creator = MakeKeyID();
    CreateTestNamespace("kept", creator);
    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    AuctionInfo auction = UserAuction("alice", "kept", creator, 5000 * COIN, T0);
    uint256 auctionId;
    BOOST_REQUIRE(aMgr->CreateAuction(auction, auctionId));
    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, bidId));
    COutPoint deposit(uint256S("d0"), 1);
    BOOST_REQUIRE(aMgr->CreateDepositLock(deposit, auctionId, bidId, bidder, 900 * COIN, 3));
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 77;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    Restart(*reddidManager);
    aMgr = reddidManager->GetAuctionManager();

    BOOST_CHECK(reddidManager->GetNamespaceManager()->IsOwner("kept", creator));
    AuctionInfo info;
    BOOST_REQUIRE(aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
    BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
    BOOST_REQUIRE_EQUAL(aMgr->GetActiveAuctions().size(), 1U);
    std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == bidId);
    BOOST_CHECK(aMgr->IsDepositLocked(deposit));
    BOOST_CHECK_EQUAL(GetLock(aMgr, deposit).lockHeight, 3);
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    BOOST_CHECK_EQUAL(config.settlementWindowBlocks, 77);

    // The auction carries on
    BOOST_CHECK(aMgr->PlaceBid(auctionId, MakeKeyID(), 7000 * COIN, bidId));
}

// SUSPECTED BUG (documented, not fixed): namespace auctions and user ID
// auctions share one key prefix in the database. NamespaceManager::Load and
// AuctionManager::Load each take every auction, whichever kind it is, and
// each manager's Save writes back every auction it holds. Before a restart
// each manager holds the auctions that were created through it. After a
// restart both hold all of them, each manager changes only its own copy, and
// each will take a bid on an auction of the other kind under its own rules.
BOOST_AUTO_TEST_CASE(after_a_restart_both_managers_hold_every_auction)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*reddidManager);

    CKeyID creator = MakeKeyID();
    CreateTestNamespace("shared", creator);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    AuctionManager* aMgr = reddidManager->GetAuctionManager();

    AuctionInfo auction = UserAuction("alice", "shared", creator, 5000 * COIN, T0);
    uint256 userIdAuction;
    BOOST_REQUIRE(aMgr->CreateAuction(auction, userIdAuction));
    const CAmount nsReserve = nsMgr->CalculateMinPrice("fresh");
    uint256 namespaceAuction;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("fresh", creator, nsReserve, 7, AUCTION_STANDARD, namespaceAuction));

    // Before the restart: each manager takes bids on its own auction only
    uint256 bidId;
    BOOST_CHECK(!nsMgr->BidOnNamespaceAuction(userIdAuction, MakeKeyID(), 5000 * COIN, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(namespaceAuction, MakeKeyID(), nsReserve, bidId));
    BOOST_CHECK(nsMgr->GetActiveAuctions().empty());
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());

    Restart(*reddidManager);
    nsMgr = reddidManager->GetNamespaceManager();
    aMgr = reddidManager->GetAuctionManager();

    // After the restart: the namespace manager takes a bid on the user ID
    // auction, from the creator of the auction, which the auction manager
    // would refuse
    BOOST_CHECK(nsMgr->BidOnNamespaceAuction(userIdAuction, creator, 5000 * COIN, bidId));
    std::vector<AuctionInfo> nsActive = nsMgr->GetActiveAuctions();
    BOOST_REQUIRE_EQUAL(nsActive.size(), 1U);
    BOOST_CHECK_EQUAL(nsActive[0].name, "alice");
    // The auction manager's copy of that auction has not changed
    BOOST_CHECK(!aMgr->IsAuctionActive(userIdAuction));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, userIdAuction).currentBid, 0);
    BOOST_CHECK(aMgr->GetAuctionBids(userIdAuction).empty());

    // And the auction manager takes a bid on the namespace auction
    CKeyID nsBidder = MakeKeyID();
    BOOST_CHECK(aMgr->PlaceBid(namespaceAuction, nsBidder, nsReserve, bidId));
    BOOST_CHECK(aMgr->IsAuctionActive(namespaceAuction));
    AuctionInfo nsCopy;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(namespaceAuction, nsCopy));
    BOOST_CHECK_EQUAL(nsCopy.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(nsCopy.currentBid, 0);
    BOOST_CHECK(nsMgr->GetAuctionBids(namespaceAuction).empty());
}

// SUSPECTED BUG (documented, not fixed): ReddIDManager::Stop() stops the
// namespace manager first and the auction manager second, and each saves
// every auction it holds. For an auction that both hold (any auction that was
// in the database when the node started) the auction manager's copy is
// written last and wins. A namespace auction is changed through the namespace
// manager only, so whatever happened to it since the node started is undone
// by the shutdown: here a bid that made it active. The bid record itself is
// stored separately and survives, so after the restart the auction is pending
// again, with no current bid, and has a bid.
BOOST_AUTO_TEST_CASE(shutdown_reverts_namespace_auctions_to_their_state_at_startup)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*reddidManager);

    CKeyID creator = MakeKeyID();
    CreateTestNamespace("shared", creator);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    AuctionManager* aMgr = reddidManager->GetAuctionManager();

    AuctionInfo auction = UserAuction("alice", "shared", creator, 5000 * COIN, T0);
    uint256 userIdAuction;
    BOOST_REQUIRE(aMgr->CreateAuction(auction, userIdAuction));
    const CAmount nsReserve = nsMgr->CalculateMinPrice("fresh");
    uint256 namespaceAuction;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("fresh", creator, nsReserve, 7, AUCTION_STANDARD, namespaceAuction));

    Restart(*reddidManager);
    nsMgr = reddidManager->GetNamespaceManager();
    aMgr = reddidManager->GetAuctionManager();

    // Each auction gets a bid through the manager it belongs to
    CKeyID userIdBidder = MakeKeyID();
    CKeyID nsBidder = MakeKeyID();
    uint256 userIdBid, nsBid;
    BOOST_REQUIRE(aMgr->PlaceBid(userIdAuction, userIdBidder, 5000 * COIN, userIdBid));
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(namespaceAuction, nsBidder, nsReserve, nsBid));

    AuctionInfo info;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(namespaceAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, nsReserve);
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->ReadAuction(namespaceAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);

    Restart(*reddidManager);
    nsMgr = reddidManager->GetNamespaceManager();
    aMgr = reddidManager->GetAuctionManager();

    // The user ID auction kept its bid
    BOOST_REQUIRE(aMgr->GetAuctionInfo(userIdAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 5000 * COIN);
    BOOST_CHECK(info.currentBidder == userIdBidder);

    // The namespace auction is back where it was when the node started
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(namespaceAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
    BOOST_CHECK(nsMgr->GetActiveAuctions().size() == 1U);
    BOOST_CHECK_EQUAL(nsMgr->GetActiveAuctions()[0].name, "alice");
    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(namespaceAuction);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == nsBid);
    BOOST_CHECK(bids[0].bidder == nsBidder);

    // A second bidder can now take the lead at the reserve price, without the 5% increment
    BOOST_CHECK(nsMgr->BidOnNamespaceAuction(namespaceAuction, MakeKeyID(), nsReserve, nsBid));
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(namespaceAuction).size(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
