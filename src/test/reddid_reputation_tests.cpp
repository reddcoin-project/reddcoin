// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/auction.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key.h>
#include <primitives/transaction.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_profile_test_util.h>
#include <uint256.h>
#include <util/time.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace reddid_test;

/** Scores are sums and products of a few doubles; they are compared to within rounding. */
#define CHECK_SCORE(actual, expected) BOOST_CHECK_SMALL(static_cast<double>(actual) - static_cast<double>(expected), 1e-9)

namespace {
/** A profile created `age` seconds ago that was last active `inactive` seconds ago. */
ReddIDProfile Aged(const std::string& reddId, int64_t age, int64_t inactive)
{
    ReddIDProfile profile = TestProfile(reddId, CKeyID());
    profile.creationTime = GetTime() - age;
    profile.lastActivityTime = GetTime() - inactive;
    return profile;
}

/** The overall score, with the weights written out. */
double Overall(double longevity, double reliability, double engagement, double verification, double auction)
{
    return longevity * 0.15 + reliability * 0.20 + engagement * 0.20 + verification * 0.20 + auction * 0.25;
}

BidderStats Deposits(int total, int completed, int forfeit = 0, int defaults = 0)
{
    BidderStats stats;
    stats.depositsTotal = total;
    stats.depositsCompleted = completed;
    stats.depositsForfeit = forfeit;
    stats.auctionDefaults = defaults;
    return stats;
}

BidderStats Auctions(int participations, int wins, int bids)
{
    BidderStats stats;
    stats.auctionParticipations = participations;
    stats.auctionWins = wins;
    stats.totalBids = bids;
    return stats;
}

/** A transaction that spends the given outpoint and has no outputs. */
CTransaction SpendOf(const COutPoint& input)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(input);
    return CTransaction(mtx);
}
} // namespace

/**
 * The reputation of a profile: the five component scores at their
 * boundaries, the promotion to community verification, and
 * ReddIDManager::CalculateReputation with the statistics of auctions, bids and
 * deposits that went through the auction manager.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_reputation_tests, ProfileTestingSetup)

// ===================================================================
// Category 1: Longevity
// ===================================================================

BOOST_AUTO_TEST_CASE(longevity_grows_with_age_up_to_a_year)
{
    AddProfile(Aged("age0", 0, 0));
    AddProfile(Aged("age1", DAY, 0));
    AddProfile(Aged("agehalf", 365 * DAY / 2, 0));
    AddProfile(Aged("age364", 364 * DAY, 0));
    AddProfile(Aged("age365", 365 * DAY, 0));
    AddProfile(Aged("age366", 366 * DAY, 0));
    AddProfile(Aged("age730", 730 * DAY, 0));

    CHECK_SCORE(Calculate("age0").longevityScore, 0.0);
    CHECK_SCORE(Calculate("age1").longevityScore, 100.0 / 365.0);
    CHECK_SCORE(Calculate("agehalf").longevityScore, 50.0);
    CHECK_SCORE(Calculate("age364").longevityScore, 364.0 / 365.0 * 100.0);
    CHECK_SCORE(Calculate("age365").longevityScore, 100.0);
    CHECK_SCORE(Calculate("age366").longevityScore, 100.0);
    CHECK_SCORE(Calculate("age730").longevityScore, 100.0);
}

BOOST_AUTO_TEST_CASE(longevity_decays_between_30_and_330_days_of_inactivity)
{
    AddProfile(Aged("idle0", 365 * DAY, 0));
    AddProfile(Aged("idle30", 365 * DAY, 30 * DAY));
    AddProfile(Aged("idle30s", 365 * DAY, 30 * DAY + 1));
    AddProfile(Aged("idle105", 365 * DAY, 105 * DAY));
    AddProfile(Aged("idle180", 365 * DAY, 180 * DAY));
    AddProfile(Aged("idle329", 365 * DAY, 330 * DAY - 1));
    AddProfile(Aged("idle330", 365 * DAY, 330 * DAY));
    AddProfile(Aged("idle331", 365 * DAY, 331 * DAY));
    AddProfile(Aged("idle365", 365 * DAY, 365 * DAY));

    CHECK_SCORE(Calculate("idle0").longevityScore, 100.0);
    // At 30 days exactly there is no decay yet
    CHECK_SCORE(Calculate("idle30").longevityScore, 100.0);
    double justOver = Calculate("idle30s").longevityScore;
    BOOST_CHECK_LT(justOver, 100.0);
    BOOST_CHECK_GT(justOver, 99.99);
    CHECK_SCORE(Calculate("idle105").longevityScore, 87.5);
    CHECK_SCORE(Calculate("idle180").longevityScore, 75.0);
    double justUnder = Calculate("idle329").longevityScore;
    BOOST_CHECK_GT(justUnder, 50.0);
    BOOST_CHECK_LT(justUnder, 50.01);
    // From 330 days on the factor stays at one half
    CHECK_SCORE(Calculate("idle330").longevityScore, 50.0);
    CHECK_SCORE(Calculate("idle331").longevityScore, 50.0);
    CHECK_SCORE(Calculate("idle365").longevityScore, 50.0);
}

// SUSPECTED BUG (documented, not fixed): the decay factor is applied to the
// uncapped age and the cap of 100 afterwards. An account of two years or more
// keeps the full score however long it has been idle.
BOOST_AUTO_TEST_CASE(longevity_decay_is_applied_before_the_cap)
{
    AddProfile(Aged("old547", 547 * DAY, 547 * DAY));
    AddProfile(Aged("old730", 730 * DAY, 730 * DAY));
    AddProfile(Aged("old3650", 3650 * DAY, 3650 * DAY));

    CHECK_SCORE(Calculate("old547").longevityScore, 547.0 / 365.0 * 100.0 * 0.5);
    CHECK_SCORE(Calculate("old730").longevityScore, 100.0);
    CHECK_SCORE(Calculate("old3650").longevityScore, 100.0);
}

BOOST_AUTO_TEST_CASE(longevity_without_activity_time_decays_from_the_creation_time)
{
    ReddIDProfile young = Aged("young", 20 * DAY, 0);
    young.lastActivityTime = 0;
    AddProfile(young);
    ReddIDProfile old = Aged("old", 365 * DAY, 0);
    old.lastActivityTime = 0;
    AddProfile(old);

    CHECK_SCORE(Calculate("young").longevityScore, 20.0 / 365.0 * 100.0);
    CHECK_SCORE(Calculate("old").longevityScore, 50.0);
}

// SUSPECTED BUG (documented, not fixed): the longevity score has no lower
// bound. Nothing checks the creation time of a profile, and one that lies in
// the future gives a negative score, which lowers the overall score.
BOOST_AUTO_TEST_CASE(longevity_of_a_creation_time_in_the_future_is_negative)
{
    ReddIDProfile tomorrow = TestProfile("tomorrow", MakeKeyID());
    tomorrow.creationTime = T0 + DAY;
    AddProfile(tomorrow);
    ReddIDProfile nextYear = TestProfile("nextyear", MakeKeyID());
    nextYear.creationTime = T0 + 365 * DAY;
    AddProfile(nextYear);
    ReddIDProfile farFuture = TestProfile("farfuture", MakeKeyID());
    farFuture.creationTime = T0 + 3650 * DAY;
    AddProfile(farFuture);

    CHECK_SCORE(Calculate("tomorrow").longevityScore, -100.0 / 365.0);

    ReddIDReputation reputation = Calculate("nextyear");
    CHECK_SCORE(reputation.longevityScore, -100.0);
    CHECK_SCORE(reputation.overallScore, Overall(-100.0, 50.0, 0.0, 0.0, 50.0));
    CHECK_SCORE(reputation.overallScore, 7.5);

    reputation = Calculate("farfuture");
    CHECK_SCORE(reputation.longevityScore, -1000.0);
    CHECK_SCORE(reputation.overallScore, -127.5);

    // The stored score is rounded towards zero when it is negative
    ReddIDReputation stored;
    BOOST_REQUIRE(db->ReadReputation("nextyear", stored));
    CHECK_SCORE(stored.longevityScore, -99.99);
    CHECK_SCORE(stored.overallScore, 7.5);
}

// Current behaviour: an activity time in the future is taken as it is and
// counts as no inactivity at all.
BOOST_AUTO_TEST_CASE(longevity_with_an_activity_time_in_the_future_does_not_decay)
{
    ReddIDProfile profile = Aged("alice", 365 * DAY, 0);
    profile.lastActivityTime = T0 + 1000 * DAY;
    AddProfile(profile);
    CHECK_SCORE(Calculate("alice").longevityScore, 100.0);
}

// SUSPECTED BUG (documented, not fixed): a profile whose creation time was
// never set counts as created in 1970 and gets the full score at once.
BOOST_AUTO_TEST_CASE(longevity_of_a_creation_time_of_zero_is_maximal)
{
    ReddIDProfile profile = TestProfile("alice", MakeKeyID());
    profile.creationTime = 0;
    profile.lastActivityTime = 0;
    AddProfile(profile);
    CHECK_SCORE(Calculate("alice").longevityScore, 100.0);

    profile = TestProfile("bob", MakeKeyID());
    profile.creationTime = 0;
    AddProfile(profile);
    CHECK_SCORE(Calculate("bob").longevityScore, 100.0);
}

// ===================================================================
// Category 2: Reliability and auction score from given statistics
// ===================================================================

BOOST_AUTO_TEST_CASE(reliability_score_boundaries)
{
    AddProfile("alice");

    CHECK_SCORE(Calculate("alice", Deposits(0, 0)).reliabilityScore, 50.0);
    CHECK_SCORE(Calculate("alice", Deposits(1, 0)).reliabilityScore, 0.0);
    CHECK_SCORE(Calculate("alice", Deposits(1, 1)).reliabilityScore, 100.0);
    CHECK_SCORE(Calculate("alice", Deposits(2, 1)).reliabilityScore, 50.0);
    CHECK_SCORE(Calculate("alice", Deposits(4, 3, 1)).reliabilityScore, 65.0);
    CHECK_SCORE(Calculate("alice", Deposits(4, 3, 1, 1)).reliabilityScore, 45.0);
    CHECK_SCORE(Calculate("alice", Deposits(5, 4, 0, 4)).reliabilityScore, 0.0);
    // Clamped at zero
    CHECK_SCORE(Calculate("alice", Deposits(5, 4, 1, 4)).reliabilityScore, 0.0);
    CHECK_SCORE(Calculate("alice", Deposits(1, 0, 1, 1)).reliabilityScore, 0.0);
}

// SUSPECTED BUG (documented, not fixed): forfeits and defaults only count while
// a deposit lock exists. Without one the score is the neutral 50, whatever the
// number of defaults.
BOOST_AUTO_TEST_CASE(reliability_score_ignores_defaults_without_deposits)
{
    AddProfile("alice");
    CHECK_SCORE(Calculate("alice", Deposits(0, 0, 3, 5)).reliabilityScore, 50.0);
}

BOOST_AUTO_TEST_CASE(auction_score_caps)
{
    AddProfile("alice");

    // Participations: 3 points each, at most 30
    CHECK_SCORE(Calculate("alice", Auctions(9, 0, 0)).auctionScore, 27.0);
    CHECK_SCORE(Calculate("alice", Auctions(10, 0, 0)).auctionScore, 30.0);
    CHECK_SCORE(Calculate("alice", Auctions(11, 0, 0)).auctionScore, 30.0);
    // Wins: 8 points each, at most 40
    CHECK_SCORE(Calculate("alice", Auctions(1, 4, 0)).auctionScore, 35.0);
    CHECK_SCORE(Calculate("alice", Auctions(1, 5, 0)).auctionScore, 43.0);
    CHECK_SCORE(Calculate("alice", Auctions(1, 6, 0)).auctionScore, 43.0);
    // Bids: 2 points each, at most 30
    CHECK_SCORE(Calculate("alice", Auctions(0, 0, 14)).auctionScore, 28.0);
    CHECK_SCORE(Calculate("alice", Auctions(0, 0, 15)).auctionScore, 30.0);
    CHECK_SCORE(Calculate("alice", Auctions(0, 0, 16)).auctionScore, 30.0);

    CHECK_SCORE(Calculate("alice", Auctions(10, 5, 15)).auctionScore, 100.0);
    CHECK_SCORE(Calculate("alice", Auctions(1000, 1000, 1000)).auctionScore, 100.0);
}

// SUSPECTED BUG (documented, not fixed): a profile without auction history has
// the neutral score of 50. Its first bid replaces that by 5.
BOOST_AUTO_TEST_CASE(auction_score_drops_from_neutral_on_the_first_participation)
{
    AddProfile("alice");

    CHECK_SCORE(Calculate("alice", Auctions(0, 0, 0)).auctionScore, 50.0);
    CHECK_SCORE(Calculate("alice", Auctions(1, 0, 1)).auctionScore, 5.0);
    CHECK_SCORE(Calculate("alice", Auctions(1, 0, 0)).auctionScore, 3.0);
    CHECK_SCORE(Calculate("alice", Auctions(0, 0, 1)).auctionScore, 2.0);
    CHECK_SCORE(Calculate("alice", Auctions(1, 1, 1)).auctionScore, 13.0);
    // Wins alone do not count as history
    CHECK_SCORE(Calculate("alice", Auctions(0, 3, 0)).auctionScore, 50.0);
}

// ===================================================================
// Category 3: Engagement
// ===================================================================

BOOST_AUTO_TEST_CASE(engagement_weights_by_type_and_direction)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 6);

    CHECK_SCORE(Calculate("hub").engagementScore, 0.0);
    Connect("hub", others[0], CONNECTION_FOLLOW);
    CHECK_SCORE(Calculate("hub").engagementScore, 1.0);
    Connect("hub", others[1], CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 2.5);
    Connect(others[2], "hub", CONNECTION_FOLLOW);
    CHECK_SCORE(Calculate("hub").engagementScore, 4.0);
    Connect(others[3], "hub", CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 6.0);

    // Endorsements count in neither direction, and a block that the profile makes does not count
    Connect("hub", others[4], CONNECTION_ENDORSE);
    Connect(others[4], "hub", CONNECTION_ENDORSE);
    Connect("hub", others[5], CONNECTION_BLOCK);
    CHECK_SCORE(Calculate("hub").engagementScore, 6.0);

    // A block that the profile receives costs 5 points
    Connect(others[5], "hub", CONNECTION_BLOCK);
    CHECK_SCORE(Calculate("hub").engagementScore, 1.0);
    Connect(others[0], "hub", CONNECTION_BLOCK);
    CHECK_SCORE(Calculate("hub").engagementScore, 0.0);

    // Connections between other profiles do not count
    Connect(others[1], others[2], CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 0.0);
}

BOOST_AUTO_TEST_CASE(engagement_outgoing_connections_are_capped_at_50)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 52);

    for (int i = 0; i < 49; i++) {
        Connect("hub", others[i], CONNECTION_FOLLOW);
    }
    CHECK_SCORE(Calculate("hub").engagementScore, 49.0);
    Connect("hub", others[49], CONNECTION_FOLLOW);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
    Connect("hub", others[50], CONNECTION_FOLLOW);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
    Connect("hub", others[51], CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
}

BOOST_AUTO_TEST_CASE(engagement_outgoing_friends_reach_the_cap_at_34)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 34);

    for (int i = 0; i < 33; i++) {
        Connect("hub", others[i], CONNECTION_FRIEND);
    }
    CHECK_SCORE(Calculate("hub").engagementScore, 49.5);
    Connect("hub", others[33], CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
}

BOOST_AUTO_TEST_CASE(engagement_incoming_connections_are_capped_at_50_and_the_total_at_100)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 60);

    for (int i = 0; i < 33; i++) {
        Connect(others[i], "hub", CONNECTION_FOLLOW);
    }
    CHECK_SCORE(Calculate("hub").engagementScore, 49.5);
    Connect(others[33], "hub", CONNECTION_FOLLOW);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
    Connect(others[34], "hub", CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);

    for (int i = 0; i < 50; i++) {
        Connect("hub", others[i], CONNECTION_FOLLOW);
    }
    CHECK_SCORE(Calculate("hub").engagementScore, 100.0);
    Connect("hub", others[50], CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 100.0);

    // Blocks are taken off the capped total
    Connect(others[51], "hub", CONNECTION_BLOCK);
    CHECK_SCORE(Calculate("hub").engagementScore, 95.0);
    Connect(others[52], "hub", CONNECTION_BLOCK);
    CHECK_SCORE(Calculate("hub").engagementScore, 90.0);
}

BOOST_AUTO_TEST_CASE(engagement_incoming_friends_reach_the_cap_at_25)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 26);

    for (int i = 0; i < 24; i++) {
        Connect(others[i], "hub", CONNECTION_FRIEND);
    }
    CHECK_SCORE(Calculate("hub").engagementScore, 48.0);
    Connect(others[24], "hub", CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
    Connect(others[25], "hub", CONNECTION_FRIEND);
    CHECK_SCORE(Calculate("hub").engagementScore, 50.0);
}

BOOST_AUTO_TEST_CASE(count_incoming_by_type)
{
    AddProfile("hub");
    std::vector<std::string> others = AddProfiles("other", 5);

    Connect(others[0], "hub", CONNECTION_FOLLOW);
    Connect(others[1], "hub", CONNECTION_FOLLOW);
    Connect(others[2], "hub", CONNECTION_FRIEND);
    Connect(others[3], "hub", CONNECTION_ENDORSE);
    Connect("hub", others[4], CONNECTION_BLOCK);
    Connect(others[0], others[1], CONNECTION_BLOCK);

    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FOLLOW), 2);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FRIEND), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_ENDORSE), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_BLOCK), 0);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType(others[4], CONNECTION_BLOCK), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType(others[1], CONNECTION_BLOCK), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("nobody", CONNECTION_FOLLOW), 0);

    // A profile has one connection to another: a new one replaces the old one
    Connect(others[0], "hub", CONNECTION_FRIEND);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FOLLOW), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FRIEND), 2);

    CKeyID owner = MakeKeyID();
    AddProfile("leaver", owner);
    Connect("leaver", "hub", CONNECTION_FOLLOW);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FOLLOW), 2);
    BOOST_REQUIRE(pMgr->RemoveConnection("leaver", "hub", owner));
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("hub", CONNECTION_FOLLOW), 1);
}

// ===================================================================
// Category 4: Verification, endorsements and promotion
// ===================================================================

BOOST_AUTO_TEST_CASE(verification_base_tiers)
{
    int i = 0;
    for (const auto& [status, score] : std::vector<std::pair<VerificationStatus, double>>{
             {VERIFICATION_NONE, 0.0}, {VERIFICATION_SELF, 20.0}, {VERIFICATION_COMMUNITY, 60.0}, {VERIFICATION_OFFICIAL, 100.0}}) {
        std::string name = "tier" + ToString(i++);
        ReddIDProfile profile = TestProfile(name, MakeKeyID());
        profile.verificationStatus = status;
        AddProfile(profile);
        CHECK_SCORE(Calculate(name).verificationScore, score);
    }
}

BOOST_AUTO_TEST_CASE(weighted_endorsement_score)
{
    AddProfile("target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 0.0);
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("nobody"), 0.0);

    // A new profile has an overall score of 50, which is worth 2 points
    AddProfile("fresh");
    Connect("fresh", "target", CONNECTION_ENDORSE);
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 2.0);

    AddEndorsers("high", 1, 80.0, "target");
    AddEndorsers("low", 1, 40.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 2.0 + 3.2 + 1.6);

    // An endorser with a score of zero adds nothing; other connections add nothing
    AddEndorsers("zero", 1, 0.0, "target");
    AddProfile("friend");
    SetOverallScore("friend", 100.0);
    Connect("friend", "target", CONNECTION_FRIEND);
    Connect("target", "friend", CONNECTION_ENDORSE);
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 6.8);
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("friend"), 2.0);

    // The score follows the reputation of the endorser
    SetOverallScore("fresh", 100.0);
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 8.8);
}

BOOST_AUTO_TEST_CASE(weighted_endorsement_score_is_capped_at_40)
{
    AddProfile("target");
    AddEndorsers("first", 9, 100.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 36.0);
    AddEndorsers("tenth", 1, 100.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 40.0);
    AddEndorsers("more", 5, 100.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 40.0);
}

// SUSPECTED BUG (documented, not fixed): nothing bounds the overall score that
// UpdateReputation stores, and the endorsement score takes it as it is. One
// endorser with a large score is worth the whole bonus, and one with a
// negative score takes points away.
BOOST_AUTO_TEST_CASE(weighted_endorsement_score_trusts_stored_scores)
{
    AddProfile("target");
    AddEndorsers("neg", 1, -500.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), -20.0);
    CHECK_SCORE(Calculate("target").verificationScore, -20.0);

    AddEndorsers("huge", 1, 100000.0, "target");
    CHECK_SCORE(pMgr->CalculateWeightedEndorsementScore("target"), 40.0);
}

BOOST_AUTO_TEST_CASE(verification_score_is_capped_at_100)
{
    ReddIDProfile official = TestProfile("official", MakeKeyID());
    official.verificationStatus = VERIFICATION_OFFICIAL;
    AddProfile(official);
    AddEndorsers("fan", 3, 100.0, "official");
    CHECK_SCORE(Calculate("official").verificationScore, 100.0);

    ReddIDProfile community = TestProfile("community", MakeKeyID());
    community.verificationStatus = VERIFICATION_COMMUNITY;
    AddProfile(community);
    AddEndorsers("peer", 9, 100.0, "community");
    CHECK_SCORE(Calculate("community").verificationScore, 96.0);
    AddEndorsers("last", 1, 100.0, "community");
    CHECK_SCORE(Calculate("community").verificationScore, 100.0);
    AddEndorsers("extra", 1, 100.0, "community");
    CHECK_SCORE(Calculate("community").verificationScore, 100.0);
}

BOOST_AUTO_TEST_CASE(promotion_needs_ten_endorsers)
{
    AddProfile("target");
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("target"));
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("nobody"));

    AddEndorsers("voter", 9, 75.0, "target");
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("target"));
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_NONE);

    // Other connections from well-regarded profiles do not count
    AddProfile("friend");
    SetOverallScore("friend", 75.0);
    Connect("friend", "target", CONNECTION_FRIEND);
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("target"));

    AddEndorsers("tenth", 1, 75.0, "target");
    BOOST_CHECK(pMgr->CheckAndPromoteVerification("target"));
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
    BOOST_CHECK_EQUAL(GetStoredProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
    // Promotion is not an update of the profile
    BOOST_CHECK_EQUAL(GetProfile("target").lastUpdated, T0);

    // Once promoted there is nothing left to promote
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("target"));
}

BOOST_AUTO_TEST_CASE(promotion_needs_endorsers_with_a_score_above_60)
{
    AddProfile("target");
    AddEndorsers("voter", 9, 75.0, "target");
    AddEndorsers("edge", 1, 60.0, "target");

    // Exactly 60 is not enough
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("target"));
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_NONE);

    SetOverallScore("edge0", 60.01);
    BOOST_CHECK(pMgr->CheckAndPromoteVerification("target"));
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);

    // Ten endorsers that have just been created, with the default score of 50, do not promote
    AddProfile("other");
    for (const std::string& name : AddProfiles("fresh", 10)) {
        Connect(name, "other", CONNECTION_ENDORSE);
    }
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("other"));
}

BOOST_AUTO_TEST_CASE(promotion_leaves_higher_statuses_alone)
{
    ReddIDProfile community = TestProfile("community", MakeKeyID());
    community.verificationStatus = VERIFICATION_COMMUNITY;
    AddProfile(community);
    ReddIDProfile official = TestProfile("official", MakeKeyID());
    official.verificationStatus = VERIFICATION_OFFICIAL;
    AddProfile(official);
    ReddIDProfile self = TestProfile("self", MakeKeyID());
    self.verificationStatus = VERIFICATION_SELF;
    AddProfile(self);

    for (const std::string& name : AddProfiles("voter", 10)) {
        SetOverallScore(name, 75.0);
        Connect(name, "community", CONNECTION_ENDORSE);
    }
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("community"));
    BOOST_CHECK_EQUAL(GetProfile("community").verificationStatus, VERIFICATION_COMMUNITY);

    AddEndorsers("fan", 10, 75.0, "official");
    BOOST_CHECK(!pMgr->CheckAndPromoteVerification("official"));
    BOOST_CHECK_EQUAL(GetProfile("official").verificationStatus, VERIFICATION_OFFICIAL);

    AddEndorsers("peer", 10, 75.0, "self");
    BOOST_CHECK(pMgr->CheckAndPromoteVerification("self"));
    BOOST_CHECK_EQUAL(GetProfile("self").verificationStatus, VERIFICATION_COMMUNITY);
}

// SUSPECTED BUG (documented, not fixed): calculating a reputation changes the
// profile. The promotion runs inside CalculateReputation and is written to the
// database, so asking for the reputation of a profile is not a read.
BOOST_AUTO_TEST_CASE(calculating_the_reputation_promotes_the_profile)
{
    ReddIDProfile target = TestProfile("target", MakeKeyID());
    target.verificationStatus = VERIFICATION_SELF;
    AddProfile(target);

    AddEndorsers("voter", 9, 75.0, "target");
    ReddIDReputation reputation = Calculate("target");
    CHECK_SCORE(reputation.verificationScore, 20.0 + 9 * 0.75 * 4.0);
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_SELF);

    AddEndorsers("tenth", 1, 75.0, "target");
    BOOST_CHECK_EQUAL(GetStoredProfile("target").verificationStatus, VERIFICATION_SELF);
    reputation = Calculate("target");
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
    BOOST_CHECK_EQUAL(GetStoredProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
    // The score of the same call is already that of the new status
    CHECK_SCORE(reputation.verificationScore, 60.0 + 10 * 0.75 * 4.0);
}

// SUSPECTED BUG (documented, not fixed): a promotion is never taken back. The
// profile stays community verified when the endorsers lose their reputation
// or withdraw their endorsement.
BOOST_AUTO_TEST_CASE(promotion_is_never_revoked)
{
    AddProfile("target");
    CKeyID voterKey = MakeKeyID();
    std::vector<std::string> voters;
    for (int i = 0; i < 10; i++) {
        voters.push_back("voter" + ToString(i));
        AddProfile(voters.back(), voterKey);
        SetOverallScore(voters.back(), 75.0);
        Connect(voters.back(), "target", CONNECTION_ENDORSE);
    }
    CHECK_SCORE(Calculate("target").verificationScore, 90.0);
    BOOST_REQUIRE_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);

    for (const std::string& voter : voters) {
        SetOverallScore(voter, 0.0);
    }
    CHECK_SCORE(Calculate("target").verificationScore, 60.0);
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);

    for (const std::string& voter : voters) {
        BOOST_REQUIRE(pMgr->RemoveConnection(voter, "target", voterKey));
    }
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("target", CONNECTION_ENDORSE), 0);
    CHECK_SCORE(Calculate("target").verificationScore, 60.0);
    BOOST_CHECK_EQUAL(GetProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
    BOOST_CHECK_EQUAL(GetStoredProfile("target").verificationStatus, VERIFICATION_COMMUNITY);
}

// Current behaviour: all ten endorsing profiles may belong to one key. (The
// case above is such a promotion.) What limits it is that a profile needs an
// overall score above 60 to count.
BOOST_AUTO_TEST_CASE(endorsers_of_one_owner_promote)
{
    CKeyID owner = MakeKeyID();
    AddProfile("target", owner);
    for (int i = 0; i < 10; i++) {
        std::string name = "sock" + ToString(i);
        AddProfile(name, owner);
        SetOverallScore(name, 61.0);
        Connect(name, "target", CONNECTION_ENDORSE);
    }
    BOOST_CHECK(pMgr->CheckAndPromoteVerification("target"));
}

// ===================================================================
// Category 5: The stored reputation
// ===================================================================

// Current behaviour: a profile starts with an overall score of 50 that no
// calculation gives. The first calculation, with nothing else changed, lowers
// it to 22.5.
BOOST_AUTO_TEST_CASE(first_calculation_replaces_the_default_reputation)
{
    AddProfile("alice");

    ReddIDReputation initial;
    BOOST_REQUIRE(pMgr->GetReputation("alice", initial));
    CHECK_SCORE(initial.overallScore, 50.0);
    CHECK_SCORE(initial.longevityScore, 0.0);
    CHECK_SCORE(initial.reliabilityScore, 50.0);
    CHECK_SCORE(initial.engagementScore, 0.0);
    CHECK_SCORE(initial.verificationScore, 0.0);
    CHECK_SCORE(initial.auctionScore, 50.0);
    BOOST_CHECK_EQUAL(initial.lastCalculated, T0);

    SetMockTime(T0 + 1);
    ReddIDReputation calculated = Calculate("alice");
    CHECK_SCORE(calculated.longevityScore, 100.0 / 365.0 / 86400.0);
    CHECK_SCORE(calculated.reliabilityScore, 50.0);
    CHECK_SCORE(calculated.engagementScore, 0.0);
    CHECK_SCORE(calculated.verificationScore, 0.0);
    CHECK_SCORE(calculated.auctionScore, 50.0);
    CHECK_SCORE(calculated.overallScore, 22.5 + 0.15 * 100.0 / 365.0 / 86400.0);
    BOOST_CHECK_EQUAL(calculated.lastCalculated, T0 + 1);
    BOOST_CHECK_EQUAL(calculated.reddId, "alice");
}

BOOST_AUTO_TEST_CASE(calculation_is_stored_in_memory_and_in_the_database)
{
    ReddIDProfile profile = Aged("alice", 73 * DAY, 0);
    profile.verificationStatus = VERIFICATION_SELF;
    AddProfile(profile);
    AddProfile("bob");
    Connect("alice", "bob", CONNECTION_FRIEND);

    BidderStats stats;
    stats.depositsTotal = 4;
    stats.depositsCompleted = 3;
    stats.auctionParticipations = 2;
    stats.auctionWins = 1;
    stats.totalBids = 3;
    ReddIDReputation calculated = Calculate("alice", stats);
    CHECK_SCORE(calculated.longevityScore, 20.0);
    CHECK_SCORE(calculated.reliabilityScore, 75.0);
    CHECK_SCORE(calculated.engagementScore, 1.5);
    CHECK_SCORE(calculated.verificationScore, 20.0);
    CHECK_SCORE(calculated.auctionScore, 20.0);
    CHECK_SCORE(calculated.overallScore, 3.0 + 15.0 + 0.3 + 4.0 + 5.0);

    for (bool fromDatabase : {false, true}) {
        ReddIDReputation reputation;
        if (fromDatabase) {
            BOOST_REQUIRE(db->ReadReputation("alice", reputation));
        } else {
            BOOST_REQUIRE(pMgr->GetReputation("alice", reputation));
        }
        BOOST_CHECK_EQUAL(reputation.reddId, "alice");
        CHECK_SCORE(reputation.overallScore, 27.3);
        CHECK_SCORE(reputation.longevityScore, 20.0);
        CHECK_SCORE(reputation.reliabilityScore, 75.0);
        CHECK_SCORE(reputation.engagementScore, 1.5);
        CHECK_SCORE(reputation.verificationScore, 20.0);
        CHECK_SCORE(reputation.auctionScore, 20.0);
        BOOST_CHECK_EQUAL(reputation.lastCalculated, T0);
    }

    // The calculation does not touch the profile
    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0);
    BOOST_CHECK_EQUAL(GetProfile("alice").lastUpdated, T0);
}

// SUSPECTED BUG (documented, not fixed): CalculateReputation stores the object
// that the caller passes in, after setting the scores. The proof and the
// signatures of the stored reputation are therefore replaced by whatever the
// caller's object held: nothing when it is a new object.
BOOST_AUTO_TEST_CASE(recalculation_overwrites_proof_and_signatures)
{
    AddProfile("alice");

    ReddIDReputation attested;
    attested.reddId = "alice";
    attested.overallScore = 88.0;
    attested.calculationProof = uint256S("a77e57");
    attested.calculatorSignatures = "[\"sig1\",\"sig2\"]";
    BOOST_REQUIRE(pMgr->UpdateReputation(attested));
    ReddIDReputation stored;
    BOOST_REQUIRE(db->ReadReputation("alice", stored));
    BOOST_CHECK(stored.calculationProof == uint256S("a77e57"));
    BOOST_CHECK_EQUAL(stored.calculatorSignatures, "[\"sig1\",\"sig2\"]");

    Calculate("alice");
    for (bool fromDatabase : {false, true}) {
        ReddIDReputation reputation;
        if (fromDatabase) {
            BOOST_REQUIRE(db->ReadReputation("alice", reputation));
        } else {
            BOOST_REQUIRE(pMgr->GetReputation("alice", reputation));
        }
        BOOST_CHECK(reputation.calculationProof.IsNull());
        BOOST_CHECK_EQUAL(reputation.calculatorSignatures, "");
        CHECK_SCORE(reputation.overallScore, 22.5);
    }

    // What the caller's object holds is stored with the new scores
    ReddIDReputation carried;
    carried.calculationProof = uint256S("0ddba11");
    carried.calculatorSignatures = "anything";
    BOOST_REQUIRE(pMgr->CalculateReputation("alice", BidderStats(), carried));
    BOOST_REQUIRE(db->ReadReputation("alice", stored));
    BOOST_CHECK(stored.calculationProof == uint256S("0ddba11"));
    BOOST_CHECK_EQUAL(stored.calculatorSignatures, "anything");
    CHECK_SCORE(stored.overallScore, 22.5);
}

// Current behaviour: UpdateReputation stores any scores for any profile. There
// is no check of who calls it or of the values.
BOOST_AUTO_TEST_CASE(update_reputation_stores_what_it_is_given)
{
    AddProfile("alice");
    ReddIDReputation reputation;
    reputation.reddId = "alice";
    reputation.overallScore = 1234.5;
    reputation.auctionScore = -7.25;
    BOOST_CHECK(pMgr->UpdateReputation(reputation));

    ReddIDReputation stored;
    BOOST_REQUIRE(pMgr->GetReputation("alice", stored));
    CHECK_SCORE(stored.overallScore, 1234.5);
    CHECK_SCORE(stored.auctionScore, -7.25);
    BOOST_REQUIRE(db->ReadReputation("alice", stored));
    CHECK_SCORE(stored.overallScore, 1234.5);
    // Rounded towards zero when negative: -7.25 is stored as -724 hundredths
    CHECK_SCORE(stored.auctionScore, -7.24);
}

// ===================================================================
// Category 6: ReddIDManager::CalculateReputation with real auctions
// ===================================================================

BOOST_AUTO_TEST_CASE(manager_calculates_neutral_scores_without_auction_history)
{
    AddProfile("alice");
    // Somebody else bids
    uint256 auctionId = NewAuction("name");
    PlaceDepositBid(auctionId, 6000 * COIN);

    ReddIDReputation reputation;
    BOOST_CHECK(reddidManager->CalculateReputation("alice", reputation));
    CHECK_SCORE(reputation.reliabilityScore, 50.0);
    CHECK_SCORE(reputation.auctionScore, 50.0);
    CHECK_SCORE(reputation.overallScore, 22.5);

    // The result is stored in the profile manager
    ReddIDReputation stored;
    BOOST_REQUIRE(pMgr->GetReputation("alice", stored));
    CHECK_SCORE(stored.overallScore, 22.5);
    BOOST_CHECK_EQUAL(stored.lastCalculated, T0);

    BOOST_CHECK(!reddidManager->CalculateReputation("nobody", reputation));
}

// SUSPECTED BUG (documented, not fixed): two things happen to the reputation
// of a user on the first bid. The deposit that is locked for the bid counts as
// a deposit but not as a completed one, so the reliability score goes from
// the neutral 50 to 0 for as long as the auction runs. And the auction score
// goes from the neutral 50 to 5. A profile that takes part in its first
// auction, and does everything right, loses nearly all of its overall score.
BOOST_AUTO_TEST_CASE(manager_first_live_bid_drops_reliability_and_auction_score)
{
    CKeyID alice = MakeKeyID();
    AddProfile("alice", alice);

    ReddIDReputation before;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", before));
    CHECK_SCORE(before.overallScore, 22.5);

    uint256 auctionId = NewAuction("name");
    DepositBid bid = PlaceDepositBid(auctionId, alice, 6000 * COIN);
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, bid.utxo).state, DEPOSIT_LOCKED_ACTIVE);

    BidderStats stats;
    BOOST_REQUIRE(aMgr->GetBidderStats(alice, stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 1);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 1);
    BOOST_CHECK_EQUAL(stats.depositsTotal, 1);
    BOOST_CHECK_EQUAL(stats.depositsCompleted, 0);

    ReddIDReputation after;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", after));
    CHECK_SCORE(after.reliabilityScore, 0.0);
    CHECK_SCORE(after.auctionScore, 5.0);
    CHECK_SCORE(after.longevityScore, 0.0);
    CHECK_SCORE(after.engagementScore, 0.0);
    CHECK_SCORE(after.verificationScore, 0.0);
    CHECK_SCORE(after.overallScore, 1.25);

    // A raised bid in the same auction: one more bid, one more live deposit
    PlaceDepositBid(auctionId, 7000 * COIN);
    PlaceDepositBid(auctionId, alice, 8000 * COIN);
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", after));
    CHECK_SCORE(after.reliabilityScore, 0.0);
    CHECK_SCORE(after.auctionScore, 7.0);
}

// SUSPECTED BUG (documented, not fixed): the statistics are looked up by the
// key that owns the profile. Bids made with any other key of the same user do
// not count, and when a profile is transferred the history stays with the
// key: the seller keeps it and the buyer brings their own.
BOOST_AUTO_TEST_CASE(manager_matches_bids_by_the_owner_key_of_the_profile)
{
    CKeyID profileKey = MakeKeyID();
    CKeyID biddingKey = MakeKeyID();
    AddProfile("alice", profileKey);

    uint256 auctionId = NewAuction("name");
    PlaceDepositBid(auctionId, biddingKey, 6000 * COIN);

    ReddIDReputation reputation;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", reputation));
    CHECK_SCORE(reputation.reliabilityScore, 50.0);
    CHECK_SCORE(reputation.auctionScore, 50.0);
    CHECK_SCORE(reputation.overallScore, 22.5);

    // Transferred to the bidding key, the profile takes on the history of that key
    BOOST_REQUIRE(pMgr->TransferProfile("alice", profileKey, biddingKey));
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", reputation));
    CHECK_SCORE(reputation.reliabilityScore, 0.0);
    CHECK_SCORE(reputation.auctionScore, 5.0);

    // And two profiles of one key share one history
    AddProfile("alice2", biddingKey);
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice2", reputation));
    CHECK_SCORE(reputation.reliabilityScore, 0.0);
    CHECK_SCORE(reputation.auctionScore, 5.0);
}

BOOST_AUTO_TEST_CASE(manager_scores_a_bid_without_deposit)
{
    CKeyID alice = MakeKeyID();
    AddProfile("alice", alice);

    uint256 auctionId = NewAuction("name");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, alice, 6000 * COIN, bidId));

    ReddIDReputation reputation;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", reputation));
    // No deposit, no deposit history
    CHECK_SCORE(reputation.reliabilityScore, 50.0);
    CHECK_SCORE(reputation.auctionScore, 5.0);
    CHECK_SCORE(reputation.overallScore, 11.25);
}

// Current behaviour: between the end of the auction and the settlement the
// reliability of the winner is 0, because the deposit is still locked.
BOOST_AUTO_TEST_CASE(manager_scores_winner_and_loser_of_a_settled_auction)
{
    CKeyID winnerKey = MakeKeyID();
    CKeyID loserKey = MakeKeyID();
    AddProfile("winner", winnerKey);
    AddProfile("loser", loserKey);

    uint256 auctionId = NewAuction("name");
    DepositBid losing = PlaceDepositBid(auctionId, loserKey, 6000 * COIN);
    DepositBid winning = PlaceDepositBid(auctionId, winnerKey, 8000 * COIN);

    EndAuction(auctionId);
    const double longevity = 7.0 / 365.0 * 100.0;
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, CTransaction(CMutableTransaction())));
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, losing.utxo).state, DEPOSIT_RELEASED_LOST);
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, winning.utxo).state, DEPOSIT_LOCKED_ACTIVE);

    // The deposit of the loser has been released: reliability 100, one participation, one bid
    ReddIDReputation loser;
    BOOST_REQUIRE(reddidManager->CalculateReputation("loser", loser));
    CHECK_SCORE(loser.longevityScore, longevity);
    CHECK_SCORE(loser.reliabilityScore, 100.0);
    CHECK_SCORE(loser.auctionScore, 5.0);
    CHECK_SCORE(loser.overallScore, Overall(longevity, 100.0, 0.0, 0.0, 5.0));

    // Current behaviour: the winner has won but has yet to settle, the deposit
    // is still locked and the reliability is 0
    ReddIDReputation winner;
    BOOST_REQUIRE(reddidManager->CalculateReputation("winner", winner));
    CHECK_SCORE(winner.reliabilityScore, 0.0);
    CHECK_SCORE(winner.auctionScore, 13.0);

    BOOST_REQUIRE(aMgr->CompleteSettlement(auctionId, winning.bidId, SpendOf(winning.utxo)));
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, winning.utxo).state, DEPOSIT_CONSUMED_WON);
    BOOST_REQUIRE(reddidManager->CalculateReputation("winner", winner));
    CHECK_SCORE(winner.longevityScore, longevity);
    CHECK_SCORE(winner.reliabilityScore, 100.0);
    CHECK_SCORE(winner.auctionScore, 13.0);
    CHECK_SCORE(winner.overallScore, Overall(longevity, 100.0, 0.0, 0.0, 13.0));

    // Stored for both
    ReddIDReputation stored;
    BOOST_REQUIRE(pMgr->GetReputation("winner", stored));
    CHECK_SCORE(stored.reliabilityScore, 100.0);
    BOOST_CHECK_EQUAL(stored.lastCalculated, T0 + 7 * DAY);
    BOOST_REQUIRE(db->ReadReputation("loser", stored));
    CHECK_SCORE(stored.reliabilityScore, 100.0);
    CHECK_SCORE(stored.auctionScore, 5.0);
}

// SUSPECTED BUG (documented, not fixed): a winner who does not pay is marked
// down only while the forfeited deposit lock exists. The forfeit record
// removes the lock, and with it every trace of the default from the
// reliability score, which returns to the neutral 50. The defaulted auction
// goes on counting as a win in the auction score.
BOOST_AUTO_TEST_CASE(manager_scores_a_default_until_the_deposit_is_forfeited)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    CKeyID alice = MakeKeyID();
    AddProfile("alice", alice);

    uint256 auctionId = NewAuction("name");
    DepositBid bid = PlaceDepositBid(auctionId, alice, 6000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, CTransaction(CMutableTransaction())));
    BOOST_REQUIRE(aMgr->ProcessDefault(auctionId, bid.bidId));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_DEFAULTED);
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, bid.utxo).state, DEPOSIT_FORFEITED);

    BidderStats stats;
    BOOST_REQUIRE(aMgr->GetBidderStats(alice, stats));
    BOOST_CHECK_EQUAL(stats.depositsTotal, 1);
    BOOST_CHECK_EQUAL(stats.depositsCompleted, 0);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 1);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 1);
    BOOST_CHECK_EQUAL(stats.auctionWins, 1);

    ReddIDReputation reputation;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", reputation));
    // 0 for the completion rate, less 10 for the forfeit and 20 for the default, clamped
    CHECK_SCORE(reputation.reliabilityScore, 0.0);
    CHECK_SCORE(reputation.auctionScore, 13.0);

    BOOST_REQUIRE(Process(TxFrom(MakeKeyID(), RecordScript(OP_DEPOSIT_FORFEIT, auctionId, bid.bidId))));
    BOOST_REQUIRE(aMgr->GetBidderStats(alice, stats));
    BOOST_CHECK_EQUAL(stats.depositsTotal, 0);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 1);

    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", reputation));
    CHECK_SCORE(reputation.reliabilityScore, 50.0);
    CHECK_SCORE(reputation.auctionScore, 13.0);
    // Reliability is back at the neutral 50, as if there had been no default;
    // the overall score is 13.25 plus longevity
    const double longevity = 7.0 / 365.0 * 100.0;
    CHECK_SCORE(reputation.overallScore, Overall(longevity, 50.0, 0.0, 0.0, 13.0));
}

// The same history through the manager and by hand gives the same scores
BOOST_AUTO_TEST_CASE(manager_passes_the_bidder_stats_of_the_auction_manager)
{
    CKeyID alice = MakeKeyID();
    ReddIDProfile profile = Aged("alice", 200 * DAY, 60 * DAY);
    profile.owner = alice;
    profile.verificationStatus = VERIFICATION_SELF;
    AddProfile(profile);
    AddProfile("bob");
    Connect("bob", "alice", CONNECTION_FRIEND);

    uint256 first = NewAuction("first");
    PlaceDepositBid(first, alice, 6000 * COIN);
    uint256 second = NewAuction("second");
    PlaceDepositBid(second, alice, 6000 * COIN);
    uint256 third = NewAuction("third");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(third, alice, 6000 * COIN, bidId));

    BidderStats stats;
    BOOST_REQUIRE(aMgr->GetBidderStats(alice, stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 3);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 3);
    BOOST_CHECK_EQUAL(stats.depositsTotal, 2);

    ReddIDReputation viaManager;
    BOOST_REQUIRE(reddidManager->CalculateReputation("alice", viaManager));
    ReddIDReputation byHand = Calculate("alice", stats);

    CHECK_SCORE(viaManager.longevityScore, byHand.longevityScore);
    CHECK_SCORE(viaManager.reliabilityScore, byHand.reliabilityScore);
    CHECK_SCORE(viaManager.engagementScore, byHand.engagementScore);
    CHECK_SCORE(viaManager.verificationScore, byHand.verificationScore);
    CHECK_SCORE(viaManager.auctionScore, byHand.auctionScore);
    CHECK_SCORE(viaManager.overallScore, byHand.overallScore);

    // 200 days old and idle for 60: the factor is 1 - (30 / 300) * 0.5
    CHECK_SCORE(viaManager.longevityScore, 200.0 / 365.0 * 100.0 * 0.95);
    CHECK_SCORE(viaManager.reliabilityScore, 0.0);
    CHECK_SCORE(viaManager.engagementScore, 2.0);
    CHECK_SCORE(viaManager.verificationScore, 20.0);
    CHECK_SCORE(viaManager.auctionScore, 15.0);
    CHECK_SCORE(viaManager.overallScore, Overall(200.0 / 365.0 * 100.0 * 0.95, 0.0, 2.0, 20.0, 15.0));
}

BOOST_AUTO_TEST_SUITE_END()
