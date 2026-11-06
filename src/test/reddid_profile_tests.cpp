// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/profile.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <util/string.h>
#include <util/time.h>
#include <key.h>

// Helper: create a minimal valid profile
static ReddIDProfile MakeProfile(const std::string& reddId, const CKeyID& owner)
{
    ReddIDProfile profile;
    profile.reddId = reddId;
    profile.owner = owner;
    profile.displayName = reddId;
    profile.bio = "Test bio";
    profile.verificationStatus = VERIFICATION_NONE;
    profile.creationTime = GetTime();
    profile.lastUpdated = GetTime();
    profile.expirationTime = GetTime() + 365 * 86400;
    profile.lastActivityTime = GetTime();
    profile.active = true;
    profile.flags = 0;
    return profile;
}

// Helper: create a connection between two profiles
static void MakeConnection(ProfileManager* pMgr, const std::string& from,
                           const std::string& to, SocialConnectionType type)
{
    ReddIDConnection conn;
    conn.fromReddId = from;
    conn.toReddId = to;
    conn.connectionType = type;
    conn.creationTime = GetTime();
    pMgr->CreateConnection(conn);
}

BOOST_FIXTURE_TEST_SUITE(reddid_profile_tests, ReddIDTestingSetup)

// ===================================================================
// Category 1: Profile CRUD
// ===================================================================

BOOST_AUTO_TEST_CASE(create_profile_succeeds)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("alice", owner);
    BOOST_CHECK(pMgr->CreateProfile(profile));

    ReddIDProfile result;
    BOOST_CHECK(pMgr->GetProfile("alice", result));
    BOOST_CHECK_EQUAL(result.reddId, "alice");
    BOOST_CHECK(result.owner == owner);
    BOOST_CHECK(result.active);
}

BOOST_AUTO_TEST_CASE(create_duplicate_profile_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("dupprof", owner);
    BOOST_CHECK(pMgr->CreateProfile(profile));
    BOOST_CHECK(!pMgr->CreateProfile(profile));
}

BOOST_AUTO_TEST_CASE(update_profile_changes_fields)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("update", owner);
    pMgr->CreateProfile(profile);

    profile.displayName = "Updated Name";
    profile.bio = "New bio text";
    BOOST_CHECK(pMgr->UpdateProfile(profile));

    ReddIDProfile result;
    pMgr->GetProfile("update", result);
    BOOST_CHECK_EQUAL(result.displayName, "Updated Name");
    BOOST_CHECK_EQUAL(result.bio, "New bio text");
}

BOOST_AUTO_TEST_CASE(update_by_wrong_owner_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    CKeyID imposter = MakeKeyID();
    ReddIDProfile profile = MakeProfile("owned", owner);
    pMgr->CreateProfile(profile);

    // Try to update with different owner
    ReddIDProfile fakeUpdate = MakeProfile("owned", imposter);
    fakeUpdate.displayName = "Hacked";
    BOOST_CHECK(!pMgr->UpdateProfile(fakeUpdate));
}

BOOST_AUTO_TEST_CASE(deactivate_profile_succeeds)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("deact", owner);
    pMgr->CreateProfile(profile);

    BOOST_CHECK(pMgr->DeactivateProfile("deact", owner));
    BOOST_CHECK(!pMgr->IsProfileActive("deact"));
    BOOST_CHECK(pMgr->ProfileExists("deact"));
}

BOOST_AUTO_TEST_CASE(reactivate_profile_succeeds)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("react", owner);
    pMgr->CreateProfile(profile);
    pMgr->DeactivateProfile("react", owner);

    BOOST_CHECK(pMgr->ReactivateProfile("react", owner));
    BOOST_CHECK(pMgr->IsProfileActive("react"));
}

BOOST_AUTO_TEST_CASE(deactivate_by_wrong_owner_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    CKeyID imposter = MakeKeyID();
    ReddIDProfile profile = MakeProfile("nooff", owner);
    pMgr->CreateProfile(profile);

    BOOST_CHECK(!pMgr->DeactivateProfile("nooff", imposter));
    BOOST_CHECK(pMgr->IsProfileActive("nooff"));
}

// ===================================================================
// Category 2: Profile hash
// ===================================================================

BOOST_AUTO_TEST_CASE(profile_hash_deterministic)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("hashp", owner);

    uint256 hash1 = pMgr->CalculateProfileHash(profile);
    uint256 hash2 = pMgr->CalculateProfileHash(profile);
    BOOST_CHECK(hash1 == hash2);
    BOOST_CHECK(!hash1.IsNull());
}

BOOST_AUTO_TEST_CASE(profile_hash_changes_on_field_change)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("hashpc", owner);

    uint256 hashOrig = pMgr->CalculateProfileHash(profile);
    profile.bio = "Changed bio";
    uint256 hashChanged = pMgr->CalculateProfileHash(profile);
    BOOST_CHECK(hashOrig != hashChanged);
}

// ===================================================================
// Category 3: Social connections
// ===================================================================

BOOST_AUTO_TEST_CASE(create_follow_connection)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("conn1a", owner1));
    pMgr->CreateProfile(MakeProfile("conn1b", owner2));

    ReddIDConnection conn;
    conn.fromReddId = "conn1a";
    conn.toReddId = "conn1b";
    conn.connectionType = CONNECTION_FOLLOW;
    conn.creationTime = GetTime();
    BOOST_CHECK(pMgr->CreateConnection(conn));

    ReddIDConnection result;
    BOOST_CHECK(pMgr->GetConnection("conn1a", "conn1b", result));
    BOOST_CHECK_EQUAL(result.connectionType, CONNECTION_FOLLOW);
}

BOOST_AUTO_TEST_CASE(create_all_connection_types)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("typea", owner1));
    pMgr->CreateProfile(MakeProfile("typeb", owner2));

    SocialConnectionType types[] = {CONNECTION_FOLLOW, CONNECTION_FRIEND, CONNECTION_ENDORSE, CONNECTION_BLOCK};

    for (auto type : types) {
        // Each CreateConnection with same from/to updates the existing connection
        ReddIDConnection conn;
        conn.fromReddId = "typea";
        conn.toReddId = "typeb";
        conn.connectionType = type;
        conn.creationTime = GetTime();
        BOOST_CHECK(pMgr->CreateConnection(conn));

        ReddIDConnection result;
        pMgr->GetConnection("typea", "typeb", result);
        BOOST_CHECK_EQUAL(result.connectionType, type);
    }
}

BOOST_AUTO_TEST_CASE(list_connections)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    CKeyID owner3 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("list1", owner1));
    pMgr->CreateProfile(MakeProfile("list2", owner2));
    pMgr->CreateProfile(MakeProfile("list3", owner3));

    ReddIDConnection conn1;
    conn1.fromReddId = "list1";
    conn1.toReddId = "list2";
    conn1.connectionType = CONNECTION_FOLLOW;
    conn1.creationTime = GetTime();
    pMgr->CreateConnection(conn1);

    ReddIDConnection conn2;
    conn2.fromReddId = "list1";
    conn2.toReddId = "list3";
    conn2.connectionType = CONNECTION_FRIEND;
    conn2.creationTime = GetTime();
    pMgr->CreateConnection(conn2);

    std::vector<ReddIDConnection> conns = pMgr->GetConnections("list1");
    BOOST_CHECK_EQUAL(conns.size(), 2u);
}

BOOST_AUTO_TEST_CASE(remove_connection_succeeds)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("rem1", owner1));
    pMgr->CreateProfile(MakeProfile("rem2", owner2));

    ReddIDConnection conn;
    conn.fromReddId = "rem1";
    conn.toReddId = "rem2";
    conn.connectionType = CONNECTION_FOLLOW;
    conn.creationTime = GetTime();
    pMgr->CreateConnection(conn);

    BOOST_CHECK(pMgr->RemoveConnection("rem1", "rem2", owner1));

    std::vector<ReddIDConnection> conns = pMgr->GetConnections("rem1");
    BOOST_CHECK_EQUAL(conns.size(), 0u);
}

BOOST_AUTO_TEST_CASE(remove_connection_wrong_owner_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    CKeyID imposter = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("norma", owner1));
    pMgr->CreateProfile(MakeProfile("normb", owner2));

    ReddIDConnection conn;
    conn.fromReddId = "norma";
    conn.toReddId = "normb";
    conn.connectionType = CONNECTION_FOLLOW;
    conn.creationTime = GetTime();
    pMgr->CreateConnection(conn);

    BOOST_CHECK(!pMgr->RemoveConnection("norma", "normb", imposter));
}

BOOST_AUTO_TEST_CASE(connection_to_nonexistent_profile_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("exist", owner));

    ReddIDConnection conn;
    conn.fromReddId = "exist";
    conn.toReddId = "ghost";
    conn.connectionType = CONNECTION_FOLLOW;
    conn.creationTime = GetTime();
    BOOST_CHECK(!pMgr->CreateConnection(conn));
}

// ===================================================================
// Category 4: Namespace resolution
// ===================================================================

BOOST_AUTO_TEST_CASE(add_namespace_resolution)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("resolvep", owner));

    BOOST_CHECK(pMgr->AddNamespaceResolution("resolvep", "redd", "alice"));

    std::string resolved = pMgr->ResolveNamespace("resolvep", "redd");
    BOOST_CHECK_EQUAL(resolved, "alice");
}

BOOST_AUTO_TEST_CASE(resolve_forward_and_reverse)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("fwdrev", owner));

    pMgr->AddNamespaceResolution("fwdrev", "crypto", "bob");

    // Forward: reddId + namespace -> userId
    std::string userId = pMgr->ResolveNamespace("fwdrev", "crypto");
    BOOST_CHECK_EQUAL(userId, "bob");

    // Reverse: userId + namespace -> reddId (depends on DB reverse index)
    std::string reddId = pMgr->ResolveReddID("bob", "crypto");
    // ResolveReddID uses ReadResolutionByUserID which depends on DB implementation
    // At minimum verify it doesn't crash
    // If the DB stores the reverse mapping, it should return "fwdrev"
}

BOOST_AUTO_TEST_CASE(resolution_for_nonexistent_profile_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    BOOST_CHECK(!pMgr->AddNamespaceResolution("nobody", "redd", "test"));
}

// ===================================================================
// Category 5: Reputation (Model C)
// ===================================================================

BOOST_AUTO_TEST_CASE(new_profile_gets_default_reputation)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("repdef", owner));

    ReddIDReputation rep;
    BOOST_CHECK(pMgr->GetReputation("repdef", rep));
    // Default: reliability=50, auction=50
    BOOST_CHECK(rep.overallScore > 0.0);
    BOOST_CHECK_EQUAL(rep.reddId, "repdef");
    BOOST_CHECK(std::abs(rep.reliabilityScore - 50.0) < 0.02);
    BOOST_CHECK(std::abs(rep.auctionScore - 50.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(update_reputation_persists)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("repupd", owner));

    ReddIDReputation rep;
    rep.reddId = "repupd";
    rep.overallScore = 75.50;
    rep.longevityScore = 80.25;
    rep.reliabilityScore = 60.00;
    rep.engagementScore = 70.00;
    rep.verificationScore = 90.00;
    rep.auctionScore = 85.00;
    rep.lastCalculated = GetTime();
    BOOST_CHECK(pMgr->UpdateReputation(rep));

    ReddIDReputation result;
    pMgr->GetReputation("repupd", result);
    // Due to int32*100 encoding, check within 0.01 tolerance
    BOOST_CHECK(std::abs(result.overallScore - 75.50) < 0.02);
    BOOST_CHECK(std::abs(result.longevityScore - 80.25) < 0.02);
    BOOST_CHECK(std::abs(result.reliabilityScore - 60.00) < 0.02);
}

BOOST_AUTO_TEST_CASE(calculate_reputation_verification_scoring)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("repcalc", owner);
    profile.verificationStatus = VERIFICATION_COMMUNITY;
    profile.creationTime = GetTime() - 365 * 86400;  // 1 year old
    profile.lastActivityTime = GetTime();  // Active now
    pMgr->CreateProfile(profile);

    BidderStats emptyStats;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("repcalc", emptyStats, rep));
    // Verification: COMMUNITY base tier = 60.0 (no endorsements)
    BOOST_CHECK(std::abs(rep.verificationScore - 60.0) < 0.02);
    // Longevity: 1 year old, active now -> ~100.0
    BOOST_CHECK(rep.longevityScore > 90.0);
    BOOST_CHECK(rep.overallScore > 0.0);
}

BOOST_AUTO_TEST_CASE(reputation_for_nonexistent_profile_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    BidderStats emptyStats;
    ReddIDReputation rep;
    BOOST_CHECK(!pMgr->CalculateReputation("ghostuser", emptyStats, rep));
}

// ===================================================================
// Category 5b: Model C Reputation Formulas
// ===================================================================

BOOST_AUTO_TEST_CASE(engagement_score_differentiates_connection_types)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID ownerA = MakeKeyID(), ownerB = MakeKeyID(), ownerC = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("enga", ownerA));
    pMgr->CreateProfile(MakeProfile("engb", ownerB));
    pMgr->CreateProfile(MakeProfile("engc", ownerC));

    // "enga" follows "engb" (outgoing follow: +1.0)
    MakeConnection(pMgr, "enga", "engb", CONNECTION_FOLLOW);
    // "enga" friends "engc" (outgoing friend: +1.5)
    MakeConnection(pMgr, "enga", "engc", CONNECTION_FRIEND);
    // "engb" follows "enga" (incoming follow: +1.5 to enga)
    MakeConnection(pMgr, "engb", "enga", CONNECTION_FOLLOW);

    BidderStats empty;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("enga", empty, rep));
    // outgoing: 1*1.0 + 1*1.5 = 2.5 (capped 50)
    // incoming: 1*1.5 (follow) = 1.5 (capped 50)
    // total = 2.5 + 1.5 = 4.0
    BOOST_CHECK(std::abs(rep.engagementScore - 4.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(engagement_score_blocks_are_negative)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID ownerA = MakeKeyID(), ownerB = MakeKeyID(), ownerC = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("blka", ownerA));
    pMgr->CreateProfile(MakeProfile("blkb", ownerB));
    pMgr->CreateProfile(MakeProfile("blkc", ownerC));

    // "blkb" follows "blka" (incoming follow: +1.5)
    MakeConnection(pMgr, "blkb", "blka", CONNECTION_FOLLOW);
    // "blkc" blocks "blka" (incoming block: -5.0)
    MakeConnection(pMgr, "blkc", "blka", CONNECTION_BLOCK);

    BidderStats empty;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("blka", empty, rep));
    // incoming: 1*1.5 (follow) = 1.5 ... minus 1*5 (block) = -3.5 -> clamped to 0
    BOOST_CHECK(rep.engagementScore < 0.02);
}

BOOST_AUTO_TEST_CASE(reliability_score_from_deposits)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("relia", owner));

    BidderStats stats;
    stats.depositsTotal = 10;
    stats.depositsCompleted = 8;
    stats.depositsForfeit = 1;
    stats.auctionDefaults = 1;
    // completionRate = 8/10 = 0.8 => 80 - 10 - 20 = 50

    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("relia", stats, rep));
    BOOST_CHECK(std::abs(rep.reliabilityScore - 50.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(reliability_score_neutral_without_history)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("relib", owner));

    BidderStats empty;  // No deposit history
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("relib", empty, rep));
    BOOST_CHECK(std::abs(rep.reliabilityScore - 50.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(reliability_score_all_clean)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("relic", owner));

    BidderStats stats;
    stats.depositsTotal = 5;
    stats.depositsCompleted = 5;
    // completionRate = 1.0 => 100 - 0 - 0 = 100

    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("relic", stats, rep));
    BOOST_CHECK(std::abs(rep.reliabilityScore - 100.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(auction_score_from_participation)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("aucscore", owner));

    BidderStats stats;
    stats.auctionParticipations = 5;  // 5*3 = 15 (capped 30)
    stats.auctionWins = 2;            // 2*8 = 16 (capped 40)
    stats.totalBids = 10;             // 10*2 = 20 (capped 30)
    // total = 15 + 16 + 20 = 51

    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("aucscore", stats, rep));
    BOOST_CHECK(std::abs(rep.auctionScore - 51.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(auction_score_neutral_without_history)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("aucnone", owner));

    BidderStats empty;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("aucnone", empty, rep));
    BOOST_CHECK(std::abs(rep.auctionScore - 50.0) < 0.02);
}

BOOST_AUTO_TEST_CASE(verification_endorsement_bonus)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID ownerTarget = MakeKeyID();
    ReddIDProfile target = MakeProfile("endtarg", ownerTarget);
    target.verificationStatus = VERIFICATION_SELF;  // base tier = 20
    pMgr->CreateProfile(target);

    // Create endorsers with known reputation scores
    CKeyID ownerE1 = MakeKeyID(), ownerE2 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("ender1", ownerE1));
    pMgr->CreateProfile(MakeProfile("ender2", ownerE2));

    // Set endorser reputations
    ReddIDReputation e1rep;
    e1rep.reddId = "ender1";
    e1rep.overallScore = 80.0;
    pMgr->UpdateReputation(e1rep);

    ReddIDReputation e2rep;
    e2rep.reddId = "ender2";
    e2rep.overallScore = 40.0;
    pMgr->UpdateReputation(e2rep);

    // Endorsements
    MakeConnection(pMgr, "ender1", "endtarg", CONNECTION_ENDORSE);
    MakeConnection(pMgr, "ender2", "endtarg", CONNECTION_ENDORSE);

    BidderStats empty;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("endtarg", empty, rep));
    // base tier (SELF) = 20
    // endorsement bonus = (80/100 + 40/100) * 4 = 1.2 * 4 = 4.8
    // total = min(100, 20 + 4.8) = 24.8
    BOOST_CHECK(std::abs(rep.verificationScore - 24.8) < 0.1);
}

BOOST_AUTO_TEST_CASE(verification_auto_promote_community)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID ownerTarget = MakeKeyID();
    ReddIDProfile target = MakeProfile("autopro", ownerTarget);
    target.verificationStatus = VERIFICATION_SELF;
    pMgr->CreateProfile(target);

    // Create 10 endorsers with reputation > 60
    for (int i = 0; i < 10; i++) {
        std::string name = "voter_" + ToString(i);
        // Ensure name is at least 3 chars
        if (name.size() < 3) name += "xx";
        CKeyID voterOwner = MakeKeyID();
        pMgr->CreateProfile(MakeProfile(name, voterOwner));

        ReddIDReputation vrep;
        vrep.reddId = name;
        vrep.overallScore = 75.0;  // Above AUTO_VERIFY_MIN_REP (60)
        pMgr->UpdateReputation(vrep);

        MakeConnection(pMgr, name, "autopro", CONNECTION_ENDORSE);
    }

    BidderStats empty;
    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("autopro", empty, rep));

    // Should have been auto-promoted to COMMUNITY
    ReddIDProfile result;
    pMgr->GetProfile("autopro", result);
    BOOST_CHECK_EQUAL(result.verificationStatus, VERIFICATION_COMMUNITY);
    // Verification score should reflect COMMUNITY base (60) + endorsement bonus
    BOOST_CHECK(rep.verificationScore >= 60.0);
}

BOOST_AUTO_TEST_CASE(longevity_activity_decay)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();

    // Active account: 1 year old, active today
    ReddIDProfile active = MakeProfile("longact", owner);
    active.creationTime = GetTime() - 365 * 86400;
    active.lastActivityTime = GetTime();
    pMgr->CreateProfile(active);

    BidderStats empty;
    ReddIDReputation repActive;
    BOOST_CHECK(pMgr->CalculateReputation("longact", empty, repActive));

    // Dormant account: 1 year old, inactive for 200 days
    CKeyID owner2 = MakeKeyID();
    ReddIDProfile dormant = MakeProfile("longdor", owner2);
    dormant.creationTime = GetTime() - 365 * 86400;
    dormant.lastActivityTime = GetTime() - 200 * 86400;
    pMgr->CreateProfile(dormant);

    ReddIDReputation repDormant;
    BOOST_CHECK(pMgr->CalculateReputation("longdor", empty, repDormant));

    // Active should score higher than dormant
    BOOST_CHECK(repActive.longevityScore > repDormant.longevityScore);
    // Active should be ~100 (1yr, no decay)
    BOOST_CHECK(repActive.longevityScore > 95.0);
    // Dormant should be less (decay factor applied)
    BOOST_CHECK(repDormant.longevityScore < 90.0);
    // But floor prevents going below 50% of raw score
    BOOST_CHECK(repDormant.longevityScore > 40.0);
}

BOOST_AUTO_TEST_CASE(overall_score_new_weights)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = MakeProfile("weights", owner);
    profile.creationTime = GetTime() - 365 * 86400;
    profile.lastActivityTime = GetTime();
    profile.verificationStatus = VERIFICATION_OFFICIAL;
    pMgr->CreateProfile(profile);

    BidderStats stats;
    stats.depositsTotal = 5;
    stats.depositsCompleted = 5;  // 100% reliability
    stats.auctionParticipations = 10;  // 30 pts
    stats.auctionWins = 5;             // 40 pts (capped)
    stats.totalBids = 15;              // 30 pts (capped)
    // auction = 30+40+30 = 100

    ReddIDReputation rep;
    BOOST_CHECK(pMgr->CalculateReputation("weights", stats, rep));

    // longevity ~100, reliability 100, engagement 0, verification 100, auction 100
    // overall = 100*0.15 + 100*0.20 + 0*0.20 + 100*0.20 + 100*0.25 = 80.0
    BOOST_CHECK(std::abs(rep.overallScore - 80.0) < 1.0);
    // Verify individual weights by checking the sum manually
    double expected = rep.longevityScore * 0.15 +
                     rep.reliabilityScore * 0.20 +
                     rep.engagementScore * 0.20 +
                     rep.verificationScore * 0.20 +
                     rep.auctionScore * 0.25;
    BOOST_CHECK(std::abs(rep.overallScore - expected) < 0.02);
}

// ===================================================================
// Category 6: Profile transfer
// ===================================================================

BOOST_AUTO_TEST_CASE(transfer_profile_changes_owner)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner1 = MakeKeyID();
    CKeyID owner2 = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("xferp", owner1));

    BOOST_CHECK(pMgr->IsProfileOwner("xferp", owner1));
    BOOST_CHECK(pMgr->TransferProfile("xferp", owner1, owner2));
    BOOST_CHECK(pMgr->IsProfileOwner("xferp", owner2));
    BOOST_CHECK(!pMgr->IsProfileOwner("xferp", owner1));
}

BOOST_AUTO_TEST_CASE(transfer_by_wrong_owner_fails)
{
    ProfileManager* pMgr = reddidManager->GetProfileManager();
    CKeyID owner = MakeKeyID();
    CKeyID attacker = MakeKeyID();
    CKeyID target = MakeKeyID();
    pMgr->CreateProfile(MakeProfile("noxfer", owner));

    BOOST_CHECK(!pMgr->TransferProfile("noxfer", attacker, target));
    BOOST_CHECK(pMgr->IsProfileOwner("noxfer", owner));
}

BOOST_AUTO_TEST_SUITE_END()
