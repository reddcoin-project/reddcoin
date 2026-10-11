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
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <scheduler.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_profile_test_util.h>
#include <test/reddid_test_fixture.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
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

/**
 * Stop the manager and bring it up again, as a node does when it is
 * restarted. The manager is started with the scheduler hidden: the periodic
 * task that Start() schedules cannot be cancelled and would outlive the
 * manager it points to.
 */
void Restart(ReddIDTestingSetup& setup)
{
    BOOST_REQUIRE(setup.reddidManager->Stop());
    BOOST_REQUIRE(setup.reddidManager->Init());
    BOOST_REQUIRE(setup.StartWithoutScheduler(*setup.reddidManager));
}

/**
 * Take the manager down the way a node does and bring a new one up. A node
 * calls Interrupt() and then Stop(), which does nothing after Interrupt(), and
 * then destroys the manager; the next run of the node constructs a new one.
 */
void RestartLikeANode(ReddIDTestingSetup& setup)
{
    setup.reddidManager->Interrupt();
    BOOST_REQUIRE(setup.reddidManager->Stop());
    BOOST_REQUIRE(setup.reddidManager->IsInitialized());
    setup.reddidManager.reset();
    setup.reddidManager = std::make_unique<ReddIDManager>(setup.m_node);
    BOOST_REQUIRE(setup.reddidManager->Init());
    BOOST_REQUIRE(setup.StartWithoutScheduler(*setup.reddidManager));
}

/** Fails the test if a line with the message is logged during the lifetime of the object. */
struct NoDebugLog : public DebugLogHelper {
    explicit NoDebugLog(const std::string& message)
        : DebugLogHelper(message, [message](const std::string* line) {
              BOOST_CHECK_MESSAGE(line == nullptr, "unexpected log line: " + message);
              return false;
          }) {}
};

/**
 * Records the order in which the component managers log that they save,
 * for the lifetime of the object.
 */
struct SaveOrder {
    std::vector<std::string> order;
    DebugLogHelper profile, auction, names;

    SaveOrder()
        : profile("Saving profile data...", Record("profile")),
          auction("Stopping AuctionManager", Record("auction")),
          names("Saving namespace data...", Record("namespace")) {}

    std::function<bool(const std::string*)> Record(const char* name) {
        return [this, name](const std::string* line) {
            if (line) order.push_back(name);
            return false;
        };
    }
};

/**
 * Replaces the scheduler of the node by one that no thread services, for the
 * lifetime of the object. Tasks given to it are kept but never run, and are
 * destroyed with it.
 *
 * The scheduler thread of the fixture is servicing its queue by now (the
 * constructor of the fixture waited for it in StartWithoutScheduler), so it
 * does not read the pointer that is replaced here.
 */
class IdleScheduler {
    NodeContext& m_node;
    std::unique_ptr<CScheduler> m_saved;

 public:
    explicit IdleScheduler(NodeContext& node) : m_node(node), m_saved(std::move(node.scheduler)) {
        m_node.scheduler = std::make_unique<CScheduler>();
    }
    ~IdleScheduler() { m_node.scheduler = std::move(m_saved); }

    size_t Tasks() const {
        std::chrono::system_clock::time_point first, last;
        return m_node.scheduler->getQueueInfo(first, last);
    }
};

/**
 * Replaces the scheduler of the node by one whose tasks run when the test
 * says so, for the lifetime of the object. No test waits for the wall clock:
 * RunTasksDueWithin() moves every task forward and returns when those that
 * became due have run.
 */
class ManualScheduler {
    NodeContext& m_node;
    std::unique_ptr<CScheduler> m_saved;

 public:
    explicit ManualScheduler(NodeContext& node) : m_node(node), m_saved(std::move(node.scheduler)) {
        m_node.scheduler = std::make_unique<CScheduler>();
        CScheduler* scheduler = m_node.scheduler.get();
        scheduler->m_service_thread = std::thread([scheduler] { scheduler->serviceQueue(); });
    }
    ~ManualScheduler() {
        m_node.scheduler->stop();
        m_node.scheduler = std::move(m_saved);
    }

    void RunTasksDueWithin(std::chrono::seconds delta) {
        m_node.scheduler->MockForward(delta);
        // Tasks run one at a time in the order of their times, so this one
        // runs after every task that the line above made due.
        std::promise<void> done;
        m_node.scheduler->scheduleFromNow([&done] { done.set_value(); }, std::chrono::milliseconds{0});
        done.get_future().wait();
    }
};
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

    ProfileManager* pMgr = reddidManager->GetProfileManager();
    BOOST_REQUIRE(pMgr != nullptr);
    BOOST_CHECK(pMgr->IsInitialized());
    BOOST_CHECK(pMgr->IsRunning());
    BOOST_CHECK(pMgr->GetDB() == reddidManager->GetReddIDDB());
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
    ProfileManager* pMgr = reddidManager->GetProfileManager();

    BOOST_CHECK(reddidManager->Init());
    BOOST_CHECK(reddidManager->Start());

    // Nothing was re-created
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == aMgr);
    BOOST_CHECK(reddidManager->GetProfileManager() == pMgr);
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
    BOOST_CHECK(manager.GetProfileManager() == nullptr);
    BOOST_CHECK(manager.GetChainState() == nullptr);

    // Without a profile manager there is no reputation to calculate
    ReddIDReputation reputation;
    BOOST_CHECK(!manager.CalculateReputation("alice", reputation));

    // Interrupt and Stop on a manager that never started are no-ops
    manager.Interrupt();
    BOOST_CHECK(manager.Stop());
    BOOST_CHECK(manager.Stop());
    BOOST_CHECK(!manager.IsInitialized());
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
    BOOST_CHECK(manager.GetProfileManager() == nullptr);
    BOOST_CHECK(!manager.Start());
    BOOST_CHECK(!manager.IsRunning());

    // Nothing was created, so there is nothing to stop
    manager.Interrupt();
    BOOST_CHECK(manager.Stop());
    BOOST_CHECK(manager.Stop());

    gArgs.ForceSetArg("-reddid", "1");
}

BOOST_AUTO_TEST_CASE(stop_releases_components)
{
    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(!reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nullptr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == nullptr);
    BOOST_CHECK(reddidManager->GetProfileManager() == nullptr);
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
    BOOST_CHECK(StartWithoutScheduler(*reddidManager));
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

    ProfileManager* pMgr = reddidManager->GetProfileManager();
    BOOST_REQUIRE(pMgr != nullptr);
    BOOST_CHECK(pMgr->IsInitialized());
    BOOST_CHECK(pMgr->IsRunning());
    BOOST_CHECK(pMgr->GetDB() == reddidManager->GetReddIDDB());
}

// Current behaviour: Interrupt() clears m_running and Stop() returns at once
// when m_running is not set. A node shuts down with Interrupt() followed by
// Stop(), so Stop() does none of its work there: it does not stop the
// component managers and does not release them. They are saved all the same,
// when the manager is destroyed, by their own destructors, each of which calls
// its Stop(). The members are destroyed in the order profile manager, auction
// manager, namespace manager, database; a plain Stop() saves in the order
// namespace, auction, profile. The order matters for an auction record that
// both the namespace manager and the auction manager hold: the copy that is
// written last wins. See destroying_an_interrupted_manager_saves_the_component_managers
// and interrupted_shutdown_reverts_user_id_auctions_to_their_state_at_startup.
BOOST_AUTO_TEST_CASE(stop_after_interrupt_does_nothing)
{
    ReddIDDB* db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    ProfileManager* pMgr = reddidManager->GetProfileManager();

    reddidManager->Interrupt();
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(!nsMgr->IsRunning());
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(!aMgr->IsRunning());
    BOOST_CHECK(aMgr->IsInitialized());
    BOOST_CHECK(!pMgr->IsRunning());
    BOOST_CHECK(pMgr->IsInitialized());

    {
        NoDebugLog noStop("Stopping ReddID manager");
        SaveOrder saved;
        BOOST_CHECK(reddidManager->Stop());
        BOOST_CHECK(reddidManager->Stop());
        BOOST_CHECK(saved.order.empty());
    }
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(reddidManager->GetAuctionManager() == aMgr);
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(nsMgr->GetDB() == db);
    BOOST_CHECK(aMgr->IsInitialized());
    BOOST_CHECK(aMgr->GetDB() == db);
    BOOST_CHECK(reddidManager->GetProfileManager() == pMgr);
    BOOST_CHECK(pMgr->IsInitialized());
    BOOST_CHECK(pMgr->GetDB() == db);
}

// Destroying an interrupted manager saves the component managers, in the
// order profile, auction, namespace.
BOOST_AUTO_TEST_CASE(destroying_an_interrupted_manager_saves_in_the_order_profile_auction_namespace)
{
    reddidManager->Interrupt();
    BOOST_REQUIRE(reddidManager->Stop());

    NoDebugLog noStop("Stopping ReddID manager");
    SaveOrder saved;
    reddidManager.reset();
    BOOST_CHECK(saved.order == (std::vector<std::string>{"profile", "auction", "namespace"}));
}

// And a plain Stop() in the order namespace, auction, profile.
BOOST_AUTO_TEST_CASE(plain_stop_saves_in_the_order_namespace_auction_profile)
{
    ASSERT_DEBUG_LOG("Stopping ReddID manager");
    SaveOrder saved;
    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(saved.order == (std::vector<std::string>{"namespace", "auction", "profile"}));
}

BOOST_AUTO_TEST_CASE(stop_twice_after_a_plain_stop)
{
    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(reddidManager->Stop());
    reddidManager->Interrupt();
    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(!reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetReddIDDB() == nullptr);
}

// Current behaviour: Stop() on a manager that was initialized but never
// started does nothing, because the manager is not running. The components
// are saved and released when the manager is destroyed.
BOOST_AUTO_TEST_CASE(stop_without_start_does_nothing)
{
    SaveOrder saved;
    {
        ReddIDManager manager(m_node);
        BOOST_REQUIRE(manager.Init());
        BOOST_CHECK(manager.IsInitialized());
        BOOST_CHECK(!manager.IsRunning());
        NamespaceManager* nsMgr = manager.GetNamespaceManager();
        BOOST_REQUIRE(nsMgr != nullptr);

        BOOST_CHECK(manager.Stop());
        BOOST_CHECK(manager.Stop());
        BOOST_CHECK(saved.order.empty());
        BOOST_CHECK(manager.IsInitialized());
        BOOST_CHECK(!manager.IsRunning());
        BOOST_CHECK(manager.GetNamespaceManager() == nsMgr);
        BOOST_CHECK(nsMgr->IsInitialized());
        BOOST_CHECK(manager.GetAuctionManager() != nullptr);
        BOOST_CHECK(manager.GetProfileManager() != nullptr);
        BOOST_CHECK(manager.GetReddIDDB() != nullptr);
    }
    BOOST_CHECK(saved.order == (std::vector<std::string>{"profile", "auction", "namespace"}));

    // The fixture's manager is not affected
    BOOST_CHECK(reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->GetNamespaceManager()->IsRunning());
}

// The destructors of the component managers save what they hold in memory.
// The namespace is taken out of the database behind the back of the namespace
// manager. Interrupt() and Stop() leave it out; it is in the database again
// once the manager has been destroyed.
BOOST_AUTO_TEST_CASE(destroying_an_interrupted_manager_saves_the_component_managers)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

    CKeyID owner = MakeKeyID();
    CreateTestNamespace("saved", owner);
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->EraseNamespace("saved"));
    BOOST_REQUIRE(!reddidManager->GetReddIDDB()->ExistsNamespace("saved"));

    reddidManager->Interrupt();
    BOOST_REQUIRE(reddidManager->Stop());
    BOOST_CHECK(!reddidManager->GetReddIDDB()->ExistsNamespace("saved"));

    reddidManager.reset();
    reddidManager = std::make_unique<ReddIDManager>(m_node);
    BOOST_REQUIRE(reddidManager->Init());
    BOOST_REQUIRE(StartWithoutScheduler(*reddidManager));

    BOOST_CHECK(reddidManager->GetReddIDDB()->ExistsNamespace("saved"));
    BOOST_CHECK(reddidManager->GetNamespaceManager()->IsOwner("saved", owner));
}

BOOST_AUTO_TEST_CASE(state_survives_interrupt_and_destruction_with_a_database_on_disk)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

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
    CKeyID profileOwner = MakeKeyID();
    BOOST_REQUIRE(reddidManager->GetProfileManager()->CreateProfile(TestProfile("alice", profileOwner)));
    UserIDInfo userId;
    userId.name = "bob";
    userId.namespaceId = "kept";
    userId.owner = bidder;
    userId.expirationTime = T0 + 365 * DAY;
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->WriteUserID("bob", "kept", userId));

    RestartLikeANode(*this);
    aMgr = reddidManager->GetAuctionManager();

    BOOST_CHECK(reddidManager->GetNamespaceManager()->IsOwner("kept", creator));
    AuctionInfo info;
    BOOST_REQUIRE(aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
    std::vector<BidInfo> bids = reddidManager->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == bidId);
    BOOST_CHECK(aMgr->IsDepositLocked(deposit));
    BOOST_CHECK(reddidManager->GetProfileManager()->IsProfileOwner("alice", profileOwner));
    UserIDInfo stored;
    BOOST_REQUIRE(reddidManager->GetUserIDInfo("bob", "kept", stored));
    BOOST_CHECK(stored.owner == bidder);
    BOOST_CHECK_EQUAL(stored.expirationTime, T0 + 365 * DAY);

    // And once more, to show that the second shutdown worked like the first
    RestartLikeANode(*this);
    BOOST_CHECK(reddidManager->GetNamespaceManager()->IsOwner("kept", creator));
    BOOST_CHECK(reddidManager->GetAuctionManager()->IsAuctionActive(auctionId));
    BOOST_CHECK(reddidManager->GetUserIDInfo("bob", "kept", stored));
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
    BOOST_CHECK(StartWithoutScheduler(*reddidManager));
    BOOST_CHECK(reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(nsMgr->IsRunning());
    BOOST_CHECK(reddidManager->GetAuctionManager()->IsRunning());
    BOOST_CHECK(reddidManager->GetProfileManager()->IsRunning());
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
// Category 1b: The periodic task
//
// Start() gives the scheduler of the node a task that runs every minute and
// calls AuctionManager::CheckExpiredAuctions, NamespaceManager::
// CheckExpiredNamespaces, ReddIDManager::CheckExpiredUserIDs and
// ProfileManager::CheckExpiredProfiles, in that order. The task holds a
// pointer to the manager and is never cancelled.
// ===================================================================

BOOST_AUTO_TEST_CASE(start_schedules_one_periodic_task)
{
    IdleScheduler scheduler(m_node);
    ReddIDManager manager(m_node);
    BOOST_REQUIRE(manager.Init());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 0U);

    BOOST_CHECK(manager.Start());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 1U);

    // Start() on a running manager does nothing
    BOOST_CHECK(manager.Start());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 1U);
}

BOOST_AUTO_TEST_CASE(start_without_a_scheduler_only_warns)
{
    ReddIDManager manager(m_node);
    BOOST_REQUIRE(manager.Init());
    {
        ASSERT_DEBUG_LOG("Warning: Scheduler not available, periodic tasks disabled");
        BOOST_CHECK(StartWithoutScheduler(manager));
    }
    BOOST_CHECK(manager.IsRunning());
}

// SUSPECTED BUG (documented, not fixed): the periodic task cannot be
// cancelled. Stop() leaves it with the scheduler, and if it ran after Stop()
// it would call through the component manager pointers that Stop() has
// released. A node is safe only because it stops the scheduler before it
// stops the manager. Starting an interrupted manager again schedules a second
// task next to the first.
BOOST_AUTO_TEST_CASE(periodic_task_is_never_cancelled)
{
    IdleScheduler scheduler(m_node);
    ReddIDManager manager(m_node);
    BOOST_REQUIRE(manager.Init());
    BOOST_REQUIRE(manager.Start());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 1U);

    manager.Interrupt();
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 1U);
    BOOST_CHECK(manager.Start());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 2U);

    manager.Interrupt();
    BOOST_CHECK(manager.Stop());
    BOOST_CHECK_EQUAL(scheduler.Tasks(), 2U);
}

BOOST_FIXTURE_TEST_CASE(periodic_task_expires_auctions_namespaces_user_ids_and_profiles, AuctionTestingSetup)
{
    ManualScheduler scheduler(m_node);
    BOOST_REQUIRE(reddidManager->Stop());
    BOOST_REQUIRE(reddidManager->Init());
    BOOST_REQUIRE(reddidManager->Start());
    aMgr = reddidManager->GetAuctionManager();
    db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ProfileManager* pMgr = reddidManager->GetProfileManager();

    // A namespace that does not expire, with a user ID that does, an auction
    // that ends after three days with a bid and one that ends without
    CreateTestNamespace("livens", creator);
    UserIDInfo carol;
    carol.name = "carol";
    carol.namespaceId = "livens";
    carol.owner = MakeKeyID();
    carol.expirationTime = T0 + 5 * DAY;
    BOOST_REQUIRE(db->WriteUserID("carol", "livens", carol));
    AuctionInfo withBid = UserAuction("frank", "livens", creator, 5000 * COIN, T0, 3 * DAY);
    AuctionInfo withoutBid = UserAuction("grace", "livens", creator, 5000 * COIN, T0, 3 * DAY);
    uint256 withBidId, withoutBidId, bidId;
    BOOST_REQUIRE(aMgr->CreateAuction(withBid, withBidId));
    BOOST_REQUIRE(aMgr->CreateAuction(withoutBid, withoutBidId));
    CKeyID bidder = MakeKeyID();
    BOOST_REQUIRE(aMgr->PlaceBid(withBidId, bidder, 6000 * COIN, bidId));

    // A namespace that expires after ten days, with a user ID that would not
    NamespaceInfo expiring;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("livens", expiring));
    expiring.id = "expns";
    expiring.expiration = T0 + 10 * DAY;
    BOOST_REQUIRE(nsMgr->CreateNamespace(expiring));
    UserIDInfo dave;
    dave.name = "dave";
    dave.namespaceId = "expns";
    dave.owner = MakeKeyID();
    dave.expirationTime = T0 + 3650 * DAY;
    BOOST_REQUIRE(db->WriteUserID("dave", "expns", dave));

    // A profile that expires after a day and one that expires after a year
    ReddIDProfile eve = TestProfile("eve", MakeKeyID());
    eve.expirationTime = T0 + DAY;
    BOOST_REQUIRE(pMgr->CreateProfile(eve));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", MakeKeyID())));

    // The first run, at T0, finds nothing to do
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    UserIDInfo userId;
    BOOST_CHECK(db->ReadUserID("carol", "livens", userId));
    BOOST_CHECK(db->ReadUserID("dave", "expns", userId));
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("expns"));
    BOOST_CHECK(pMgr->IsProfileActive("eve"));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, withBidId).state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, withoutBidId).state, AUCTION_PENDING);

    // Three days on the auctions have ended and the profile has expired
    SetMockTime(T0 + 3 * DAY);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    BOOST_CHECK_EQUAL(GetAuction(aMgr, withBidId).state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, withoutBidId).state, AUCTION_CANCELED);
    BOOST_REQUIRE(db->ReadUserID("frank", "livens", userId));
    BOOST_CHECK(userId.owner == bidder);
    BOOST_CHECK(!pMgr->IsProfileActive("eve"));
    BOOST_CHECK(pMgr->IsProfileActive("alice"));
    BOOST_CHECK(db->ReadUserID("carol", "livens", userId));
    BOOST_CHECK(db->ReadUserID("dave", "expns", userId));

    // The user ID is erased once its grace period of 30 days is over
    SetMockTime(T0 + 35 * DAY);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    BOOST_CHECK(db->ReadUserID("carol", "livens", userId));
    SetMockTime(T0 + 35 * DAY + 1);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    BOOST_CHECK(!db->ReadUserID("carol", "livens", userId));
    BOOST_CHECK(db->ReadUserID("dave", "expns", userId));
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("expns"));

    // And the namespace, with the user ID in it, once its own is over
    SetMockTime(T0 + 40 * DAY + 1);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("expns"));
    BOOST_CHECK(!db->ExistsNamespace("expns"));
    BOOST_CHECK(!db->ReadUserID("dave", "expns", userId));
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("livens"));
    BOOST_CHECK(db->ReadUserID("frank", "livens", userId));
    BOOST_CHECK(pMgr->IsProfileActive("alice"));
}

// SUSPECTED BUG (documented, not fixed): what the periodic task does depends
// on the clock of the node and on when the task happens to run, not on the
// chain. It changes the same records that transactions in blocks change, and
// nothing it does is undone when blocks are disconnected. Two nodes with
// different clocks, or one node that was offline, hold different state for
// the same chain. For an auction with deposits the task also takes the path
// for auctions without them (see reddid_settlement_tests,
// expiry_of_an_auction_with_deposits_bypasses_settlement): the name is
// registered to the highest bidder with no settlement and the deposits stay
// locked.
BOOST_FIXTURE_TEST_CASE(periodic_task_finalizes_an_auction_with_deposits_without_settlement, AuctionTestingSetup)
{
    ManualScheduler scheduler(m_node);
    BOOST_REQUIRE(reddidManager->Stop());
    BOOST_REQUIRE(reddidManager->Init());
    BOOST_REQUIRE(reddidManager->Start());
    aMgr = reddidManager->GetAuctionManager();
    db = reddidManager->GetReddIDDB();
    CreateTestNamespace(NS, creator);

    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);

    // One second before the end nothing happens
    SetMockTime(T0 + 7 * DAY - 1);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);

    EndAuction(auctionId);
    scheduler.RunTasksDueWithin(std::chrono::seconds{60});

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 0);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == winner.bidder);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK(aMgr->IsDepositLocked(loser.utxo));

    // The finalize record of the creator, which would have begun the settlement, comes too late
    BOOST_CHECK(reddidManager->ProcessTransaction(CTransaction(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))), 1));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_FINALIZED);
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
    Restart(*this);

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

    Restart(*this);
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
    Restart(*this);

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

    Restart(*this);
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

// SUSPECTED BUG (documented, not fixed): a plain ReddIDManager::Stop(), with
// no Interrupt() before it, stops the namespace manager first and the auction
// manager second, and each saves every auction it holds. For an auction that both hold (any auction that was
// in the database when the node started) the auction manager's copy is
// written last and wins. A namespace auction is changed through the namespace
// manager only, so whatever happened to it since the node started is undone
// by the shutdown: here a bid that made it active. The bid record itself is
// stored separately and survives, so after the restart the auction is pending
// again, with no current bid, and has a bid. A node does not shut down this
// way: see interrupted_shutdown_reverts_user_id_auctions_to_their_state_at_startup.
BOOST_AUTO_TEST_CASE(shutdown_reverts_namespace_auctions_to_their_state_at_startup)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

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

    Restart(*this);
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

    Restart(*this);
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

// SUSPECTED BUG (documented, not fixed): a node shuts down with Interrupt(),
// a Stop() that does nothing, and the destruction of the manager. The
// component managers are then saved by their destructors, the auction manager
// before the namespace manager, so for an auction that both hold the
// namespace manager's copy is written last and wins: the reverse of
// shutdown_reverts_namespace_auctions_to_their_state_at_startup. A user ID
// auction is changed through the auction manager only, so whatever happened
// to it since the node started is undone by the shutdown of the node, while
// the namespace auction keeps its bid.
BOOST_AUTO_TEST_CASE(interrupted_shutdown_reverts_user_id_auctions_to_their_state_at_startup)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

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

    // Both managers hold both auctions once they were loaded from the database
    RestartLikeANode(*this);
    nsMgr = reddidManager->GetNamespaceManager();
    aMgr = reddidManager->GetAuctionManager();

    // Each auction gets a bid through the manager it belongs to
    CKeyID userIdBidder = MakeKeyID();
    CKeyID nsBidder = MakeKeyID();
    uint256 userIdBid, nsBid;
    BOOST_REQUIRE(aMgr->PlaceBid(userIdAuction, userIdBidder, 5000 * COIN, userIdBid));
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(namespaceAuction, nsBidder, nsReserve, nsBid));

    AuctionInfo info;
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->ReadAuction(userIdAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 5000 * COIN);

    RestartLikeANode(*this);
    nsMgr = reddidManager->GetNamespaceManager();
    aMgr = reddidManager->GetAuctionManager();

    // The namespace auction kept its bid
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(namespaceAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, nsReserve);
    BOOST_CHECK(info.currentBidder == nsBidder);

    // The user ID auction is back where it was when the node started, in the
    // database and in the auction manager; its bid record is still there
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->ReadAuction(userIdAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_REQUIRE(aMgr->GetAuctionInfo(userIdAuction, info));
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
    BOOST_CHECK(!aMgr->IsAuctionActive(userIdAuction));
    std::vector<BidInfo> bids = aMgr->GetAuctionBids(userIdAuction);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == userIdBid);
    BOOST_CHECK(bids[0].bidder == userIdBidder);
}

// ===================================================================
// Category 4: Profiles and reputations
// ===================================================================

// Current behaviour: like the auction manager, the profile manager does not
// look at whether it is running. After Interrupt() it still takes profiles
// and connections and calculates reputations.
BOOST_AUTO_TEST_CASE(interrupted_profile_manager_still_takes_profiles)
{
    MockTimeGuard mockTime(T0);
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", MakeKeyID())));

    reddidManager->Interrupt();
    BOOST_CHECK(!pMgr->IsRunning());

    BOOST_CHECK(pMgr->CreateProfile(TestProfile("bob", MakeKeyID())));
    BOOST_CHECK(pMgr->CreateConnection(TestConnection("bob", "alice", CONNECTION_FOLLOW)));
    ReddIDReputation reputation;
    BOOST_CHECK(reddidManager->CalculateReputation("alice", reputation));
    BOOST_CHECK_EQUAL(reputation.engagementScore, 1.5);
}

BOOST_AUTO_TEST_CASE(calculate_reputation_needs_an_initialized_manager)
{
    MockTimeGuard mockTime(T0);
    BOOST_REQUIRE(reddidManager->GetProfileManager()->CreateProfile(TestProfile("alice", MakeKeyID())));

    ReddIDReputation reputation;
    BOOST_CHECK(reddidManager->CalculateReputation("alice", reputation));
    BOOST_CHECK(!reddidManager->CalculateReputation("nobody", reputation));

    BOOST_REQUIRE(reddidManager->Stop());
    BOOST_CHECK(!reddidManager->CalculateReputation("alice", reputation));
}

BOOST_AUTO_TEST_CASE(profile_state_survives_a_restart_with_a_database_on_disk)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID alice = MakeKeyID();
    CKeyID bob = MakeKeyID();
    ReddIDProfile created = TestProfile("alice", alice);
    created.displayName = "Alice";
    created.avatarHash = uint256S("a1");
    created.emailHash = uint256S("e1");
    created.socialData = "{\"x\":1}";
    created.messagingPubkey = {2, 3, 4};
    created.verificationStatus = VERIFICATION_SELF;
    created.flags = 5;
    BOOST_REQUIRE(pMgr->CreateProfile(created));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("bob", bob)));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("carol", bob)));
    BOOST_REQUIRE(pMgr->DeactivateProfile("carol", bob));

    ReddIDConnection friendship = TestConnection("alice", "bob", CONNECTION_FRIEND);
    friendship.visibility = 1;
    friendship.metadata = "kept";
    BOOST_REQUIRE(pMgr->CreateConnection(friendship));
    BOOST_REQUIRE(pMgr->CreateConnection(TestConnection("bob", "alice", CONNECTION_ENDORSE)));
    BOOST_REQUIRE(pMgr->CreateConnection(TestConnection("carol", "alice", CONNECTION_FOLLOW)));
    BOOST_REQUIRE(pMgr->AddNamespaceResolution("alice", "redd", "al"));
    BOOST_REQUIRE(pMgr->AddNamespaceResolution("alice", "crypto", "alice1"));

    ReddIDReputation attested;
    attested.reddId = "bob";
    attested.overallScore = 80.0;
    attested.calculationProof = uint256S("a77e57");
    attested.calculatorSignatures = "sigs";
    attested.lastCalculated = T0 - 5;
    BOOST_REQUIRE(pMgr->UpdateReputation(attested));
    ReddIDReputation before;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", before));
    uint256 hashBefore = pMgr->CalculateProfileHash(created);

    Restart(*this);
    pMgr = reddidManager->GetProfileManager();
    BOOST_REQUIRE(pMgr != nullptr);

    ReddIDProfile profile;
    BOOST_REQUIRE(pMgr->GetProfile("alice", profile));
    BOOST_CHECK(pMgr->CalculateProfileHash(profile) == hashBefore);
    BOOST_CHECK_EQUAL(profile.displayName, "Alice");
    BOOST_CHECK_EQUAL(profile.verificationStatus, VERIFICATION_SELF);
    BOOST_CHECK_EQUAL(static_cast<int>(profile.flags), 5);
    BOOST_CHECK(pMgr->IsProfileOwner("alice", alice));
    BOOST_CHECK(pMgr->IsProfileActive("alice"));
    BOOST_CHECK(pMgr->IsProfileActive("bob"));
    BOOST_CHECK(!pMgr->IsProfileActive("carol"));
    std::vector<std::string> owned;
    BOOST_CHECK(pMgr->GetProfilesByOwner(bob, owned));
    BOOST_CHECK_EQUAL(owned.size(), 2U);

    ReddIDConnection connection;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK_EQUAL(connection.connectionType, CONNECTION_FRIEND);
    BOOST_CHECK_EQUAL(connection.visibility, 1);
    BOOST_CHECK_EQUAL(connection.metadata, "kept");
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("alice", CONNECTION_ENDORSE), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("alice", CONNECTION_FOLLOW), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("bob", CONNECTION_FRIEND), 1);

    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "redd"), "al");
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "crypto"), "alice1");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("alice1", "crypto"), "alice");
    BOOST_CHECK(pMgr->UpdateNamespaceResolution("alice", "redd", "al2"));

    ReddIDReputation reputation;
    BOOST_REQUIRE(pMgr->GetReputation("bob", reputation));
    BOOST_CHECK_EQUAL(reputation.overallScore, 80.0);
    BOOST_CHECK(reputation.calculationProof == uint256S("a77e57"));
    BOOST_CHECK_EQUAL(reputation.calculatorSignatures, "sigs");
    BOOST_CHECK_EQUAL(reputation.lastCalculated, T0 - 5);

    // The same inputs give the same reputation after the restart
    BOOST_REQUIRE(pMgr->GetReputation("alice", reputation));
    BOOST_CHECK_SMALL(reputation.overallScore - before.overallScore, 0.005);
    ReddIDReputation after;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", after));
    BOOST_CHECK_SMALL(after.overallScore - before.overallScore, 1e-9);
    // SELF, endorsed by bob (80), followed by carol and with one friend
    BOOST_CHECK_SMALL(after.verificationScore - 23.2, 1e-9);
    BOOST_CHECK_SMALL(after.engagementScore - 3.0, 1e-9);

    // The profiles carry on
    BOOST_CHECK(!pMgr->CreateProfile(TestProfile("alice", MakeKeyID())));
    created.bio = "After the restart";
    BOOST_CHECK(pMgr->UpdateProfile(created));
}

// What ProfileManager::Stop saves is what every operation has saved already;
// a profile that was erased stays erased.
BOOST_AUTO_TEST_CASE(erased_profile_is_not_restored_by_a_restart)
{
    DatabaseOnDisk onDisk;
    MockTimeGuard mockTime(T0);
    Restart(*this);

    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", owner)));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("bob", owner)));
    BOOST_REQUIRE(pMgr->CreateConnection(TestConnection("alice", "bob", CONNECTION_FOLLOW)));
    BOOST_REQUIRE(pMgr->CreateConnection(TestConnection("bob", "alice", CONNECTION_FOLLOW)));
    BOOST_REQUIRE(pMgr->RemoveConnection("bob", "alice", owner));

    CMutableTransaction mtx;
    mtx.vout.emplace_back(0, CreateProfileUpdateOpReturn("alice", uint256()));
    BOOST_REQUIRE(pMgr->UndoTransaction(CTransaction(mtx), 1));
    BOOST_REQUIRE(!pMgr->ProfileExists("alice"));

    Restart(*this);
    pMgr = reddidManager->GetProfileManager();

    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));
    BOOST_CHECK(pMgr->GetConnections("bob").empty());
    // SUSPECTED BUG (documented, not fixed): the reputation and the connections
    // of the erased profile are still in the database. They are no longer
    // loaded, but they are found by the lookups that fall back to the database.
    ReddIDReputation reputation;
    BOOST_CHECK(pMgr->GetReputation("alice", reputation));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    // The connections that are loaded are those of the profiles that exist
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("bob", CONNECTION_FOLLOW), 0);
}

BOOST_AUTO_TEST_SUITE_END()
