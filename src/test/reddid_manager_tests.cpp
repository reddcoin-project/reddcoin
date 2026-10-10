// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <util/system.h>
#include <validation.h>

#include <memory>

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
}

BOOST_AUTO_TEST_CASE(init_and_start_are_idempotent)
{
    ReddIDDB* db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();

    BOOST_CHECK(reddidManager->Init());
    BOOST_CHECK(reddidManager->Start());

    // Nothing was re-created
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
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
}

// SUSPECTED BUG (documented, not fixed): Interrupt() clears m_running and
// Stop() returns early when m_running is false. A node shuts down with
// Interrupt() followed by Stop(), so Stop() does none of its work in that
// sequence: the namespace manager is not stopped (and so not saved) and the
// components are not released until the manager is destroyed.
BOOST_AUTO_TEST_CASE(stop_after_interrupt_does_nothing)
{
    ReddIDDB* db = reddidManager->GetReddIDDB();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();

    reddidManager->Interrupt();
    BOOST_CHECK(!reddidManager->IsRunning());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(!nsMgr->IsRunning());
    BOOST_CHECK(nsMgr->IsInitialized());

    BOOST_CHECK(reddidManager->Stop());
    BOOST_CHECK(reddidManager->IsInitialized());
    BOOST_CHECK(reddidManager->GetReddIDDB() == db);
    BOOST_CHECK(reddidManager->GetNamespaceManager() == nsMgr);
    BOOST_CHECK(nsMgr->IsInitialized());
    BOOST_CHECK(nsMgr->GetDB() == db);
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

BOOST_AUTO_TEST_SUITE_END()
