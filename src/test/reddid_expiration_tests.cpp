// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid_db.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <key.h>

// Helper: create a namespace with a specific expiration time
static void CreateNamespaceWithExpiration(NamespaceManager* nsMgr, const std::string& nsId,
                                          const CKeyID& owner, int64_t expiration, int gracePeriod = 30)
{
    NamespaceInfo ns;
    ns.id = nsId;
    ns.owner = owner;
    ns.allowNumbers = true;
    ns.allowHyphens = true;
    ns.allowUnderscores = true;
    ns.minLength = 3;
    ns.maxLength = 32;
    ns.renewalPeriod = 365;
    ns.gracePeriod = gracePeriod;
    ns.namespaceRevenuePct = 10;
    ns.burnPct = 70;
    ns.nodePct = 15;
    ns.devPct = 5;
    ns.minAuctionDuration = 3;
    ns.maxAuctionDuration = 7;
    ns.minBidIncrement = 5.0;
    ns.expiration = expiration;
    ns.lastUpdated = GetTime();
    ns.configHash = nsMgr->CalculateConfigHash(ns);

    bool ok = nsMgr->CreateNamespace(ns);
    assert(ok);
}

// Helper: create a minimal valid profile
static ReddIDProfile MakeTestProfile(const std::string& reddId, const CKeyID& owner,
                                     int64_t expirationTime)
{
    ReddIDProfile profile;
    profile.reddId = reddId;
    profile.owner = owner;
    profile.displayName = reddId;
    profile.bio = "Test bio";
    profile.verificationStatus = VERIFICATION_NONE;
    profile.creationTime = GetTime();
    profile.lastUpdated = GetTime();
    profile.expirationTime = expirationTime;
    profile.active = true;
    profile.flags = 0;
    return profile;
}

BOOST_FIXTURE_TEST_SUITE(reddid_expiration_tests, ReddIDTestingSetup)

// ===================================================================
// Category 1: Namespace expiration
// ===================================================================

BOOST_AUTO_TEST_CASE(check_expired_namespaces_removes_expired)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();

    // Expiration was 100 days ago, grace period is 30 days → fully expired
    int64_t pastExpiration = GetTime() - (100 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "expiredns", owner, pastExpiration, 30);

    NamespaceInfo result;
    BOOST_CHECK(nsMgr->GetNamespaceInfo("expiredns", result));

    nsMgr->CheckExpiredNamespaces();

    BOOST_CHECK(!nsMgr->GetNamespaceInfo("expiredns", result));
}

BOOST_AUTO_TEST_CASE(check_expired_namespaces_keeps_active)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();

    // Expiration is 100 days in the future
    int64_t futureExpiration = GetTime() + (100 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "activens", owner, futureExpiration, 30);

    nsMgr->CheckExpiredNamespaces();

    NamespaceInfo result;
    BOOST_CHECK(nsMgr->GetNamespaceInfo("activens", result));
    BOOST_CHECK_EQUAL(result.id, "activens");
}

BOOST_AUTO_TEST_CASE(check_expired_namespaces_keeps_in_grace)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();

    // Expiration was 10 days ago, but grace period is 30 days → still in grace
    int64_t recentExpiration = GetTime() - (10 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "gracens", owner, recentExpiration, 30);

    nsMgr->CheckExpiredNamespaces();

    NamespaceInfo result;
    BOOST_CHECK(nsMgr->GetNamespaceInfo("gracens", result));
}

BOOST_AUTO_TEST_CASE(namespace_just_past_expiry_but_in_grace_not_expired)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();

    // Expiration was exactly 29 days ago, grace period is 30 days → still safe
    int64_t borderlineExpiration = GetTime() - (29 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "borderns", owner, borderlineExpiration, 30);

    nsMgr->CheckExpiredNamespaces();

    NamespaceInfo result;
    BOOST_CHECK(nsMgr->GetNamespaceInfo("borderns", result));
}

// ===================================================================
// Category 2: User ID expiration
// ===================================================================

BOOST_AUTO_TEST_CASE(check_expired_user_ids_removes_expired)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("uidns", owner);

    ReddIDDB* db = reddidManager->GetReddIDDB();
    UserIDInfo info;
    info.name = "alice";
    info.namespaceId = "uidns";
    info.owner = owner;
    info.registrationTime = GetTime() - (400 * 24 * 60 * 60);
    // Expired 100 days ago; grace period is 30 days → fully expired
    info.expirationTime = GetTime() - (100 * 24 * 60 * 60);
    info.transactionCount = 0;
    db->WriteUserID("alice", "uidns", info);

    // Verify it exists
    UserIDInfo readBack;
    BOOST_CHECK(db->ReadUserID("alice", "uidns", readBack));

    reddidManager->CheckExpiredUserIDs();

    BOOST_CHECK(!db->ReadUserID("alice", "uidns", readBack));
}

BOOST_AUTO_TEST_CASE(check_expired_user_ids_keeps_active)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("uidns2", owner);

    ReddIDDB* db = reddidManager->GetReddIDDB();
    UserIDInfo info;
    info.name = "bob";
    info.namespaceId = "uidns2";
    info.owner = owner;
    info.registrationTime = GetTime();
    // Expires 100 days from now
    info.expirationTime = GetTime() + (100 * 24 * 60 * 60);
    info.transactionCount = 0;
    db->WriteUserID("bob", "uidns2", info);

    reddidManager->CheckExpiredUserIDs();

    UserIDInfo readBack;
    BOOST_CHECK(db->ReadUserID("bob", "uidns2", readBack));
    BOOST_CHECK_EQUAL(readBack.name, "bob");
}

BOOST_AUTO_TEST_CASE(user_id_zero_expiration_not_expired)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("uidns3", owner);

    ReddIDDB* db = reddidManager->GetReddIDDB();
    UserIDInfo info;
    info.name = "carol";
    info.namespaceId = "uidns3";
    info.owner = owner;
    info.registrationTime = GetTime();
    info.expirationTime = 0;  // no expiration
    info.transactionCount = 0;
    db->WriteUserID("carol", "uidns3", info);

    reddidManager->CheckExpiredUserIDs();

    UserIDInfo readBack;
    BOOST_CHECK(db->ReadUserID("carol", "uidns3", readBack));
}

// ===================================================================
// Category 3: Profile expiration
// ===================================================================

BOOST_AUTO_TEST_CASE(check_expired_profiles_deactivates)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();

    // Profile expired 10 days ago
    int64_t pastExpiry = GetTime() - (10 * 24 * 60 * 60);
    ReddIDProfile profile = MakeTestProfile("expuser", owner, pastExpiry);
    BOOST_CHECK(pMgr->CreateProfile(profile));
    BOOST_CHECK(pMgr->IsProfileActive("expuser"));

    pMgr->CheckExpiredProfiles();

    BOOST_CHECK(!pMgr->IsProfileActive("expuser"));

    // Profile still exists (deactivated, not deleted)
    ReddIDProfile result;
    BOOST_CHECK(pMgr->GetProfile("expuser", result));
    BOOST_CHECK(!result.active);
}

BOOST_AUTO_TEST_CASE(check_expired_profiles_keeps_active)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();

    // Profile expires 100 days from now
    int64_t futureExpiry = GetTime() + (100 * 24 * 60 * 60);
    ReddIDProfile profile = MakeTestProfile("actuser", owner, futureExpiry);
    BOOST_CHECK(pMgr->CreateProfile(profile));

    pMgr->CheckExpiredProfiles();

    BOOST_CHECK(pMgr->IsProfileActive("actuser"));
}

BOOST_AUTO_TEST_CASE(profile_zero_expiration_not_expired)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();

    // expirationTime = 0 means no expiration
    ReddIDProfile profile = MakeTestProfile("noexpuser", owner, 0);
    BOOST_CHECK(pMgr->CreateProfile(profile));

    pMgr->CheckExpiredProfiles();

    BOOST_CHECK(pMgr->IsProfileActive("noexpuser"));
}

BOOST_AUTO_TEST_CASE(already_inactive_profile_not_reprocessed)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();

    int64_t pastExpiry = GetTime() - (10 * 24 * 60 * 60);
    ReddIDProfile profile = MakeTestProfile("inactuser", owner, pastExpiry);
    profile.active = false;  // already inactive
    BOOST_CHECK(pMgr->CreateProfile(profile));

    // Should not crash or double-process
    pMgr->CheckExpiredProfiles();

    ReddIDProfile result;
    BOOST_CHECK(pMgr->GetProfile("inactuser", result));
    BOOST_CHECK(!result.active);
}

// ===================================================================
// Category 4: Cascade expiration
// ===================================================================

BOOST_AUTO_TEST_CASE(expire_namespace_cascades_user_ids)
{
    CKeyID owner = MakeKeyID();

    // Create namespace that is past expiration + grace
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    int64_t pastExpiration = GetTime() - (100 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "cascns", owner, pastExpiration, 30);

    // Add user IDs to this namespace via the DB
    ReddIDDB* db = reddidManager->GetReddIDDB();
    for (const auto& name : {"alice", "bob", "carol"}) {
        UserIDInfo info;
        info.name = name;
        info.namespaceId = "cascns";
        info.owner = owner;
        info.registrationTime = GetTime();
        info.expirationTime = GetTime() + (365 * 24 * 60 * 60); // would be active normally
        info.transactionCount = 0;
        db->WriteUserID(name, "cascns", info);
    }

    // Verify user IDs exist
    UserIDInfo readBack;
    BOOST_CHECK(db->ReadUserID("alice", "cascns", readBack));
    BOOST_CHECK(db->ReadUserID("bob", "cascns", readBack));
    BOOST_CHECK(db->ReadUserID("carol", "cascns", readBack));

    // Expire the namespace (should cascade)
    BOOST_CHECK(nsMgr->ExpireNamespace("cascns"));

    // Namespace gone
    NamespaceInfo nsResult;
    BOOST_CHECK(!nsMgr->GetNamespaceInfo("cascns", nsResult));

    // All user IDs also gone
    BOOST_CHECK(!db->ReadUserID("alice", "cascns", readBack));
    BOOST_CHECK(!db->ReadUserID("bob", "cascns", readBack));
    BOOST_CHECK(!db->ReadUserID("carol", "cascns", readBack));
}

BOOST_AUTO_TEST_CASE(check_expired_namespaces_cascades_user_ids)
{
    CKeyID owner = MakeKeyID();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();

    // Create namespace that is fully expired
    int64_t pastExpiration = GetTime() - (100 * 24 * 60 * 60);
    CreateNamespaceWithExpiration(nsMgr, "autocasc", owner, pastExpiration, 30);

    // Add a user ID
    UserIDInfo info;
    info.name = "dave";
    info.namespaceId = "autocasc";
    info.owner = owner;
    info.registrationTime = GetTime();
    info.expirationTime = GetTime() + (365 * 24 * 60 * 60);
    info.transactionCount = 0;
    db->WriteUserID("dave", "autocasc", info);

    // Run the automated check
    nsMgr->CheckExpiredNamespaces();

    // Namespace and user ID both gone
    NamespaceInfo nsResult;
    BOOST_CHECK(!nsMgr->GetNamespaceInfo("autocasc", nsResult));

    UserIDInfo readBack;
    BOOST_CHECK(!db->ReadUserID("dave", "autocasc", readBack));
}

BOOST_AUTO_TEST_SUITE_END()
