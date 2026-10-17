// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_REDDID_PROFILE_TEST_UTIL_H
#define BITCOIN_TEST_REDDID_PROFILE_TEST_UTIL_H

#include <id/profile.h>
#include <id/reddid.h>
#include <key.h>
#include <test/reddid_auction_test_util.h>
#include <util/string.h>
#include <util/time.h>

#include <cassert>
#include <string>
#include <vector>

namespace reddid_test {

/** A valid profile created now, active now, that expires in a year. */
inline ReddIDProfile TestProfile(const std::string& reddId, const CKeyID& owner) {
    ReddIDProfile profile;
    profile.reddId = reddId;
    profile.owner = owner;
    profile.displayName = reddId;
    profile.bio = "Test bio";
    profile.verificationStatus = VERIFICATION_NONE;
    profile.creationTime = GetTime();
    profile.lastUpdated = GetTime();
    profile.expirationTime = GetTime() + 365 * DAY;
    profile.lastActivityTime = GetTime();
    profile.active = true;
    profile.flags = 0;
    return profile;
}

/** A public connection made now. */
inline ReddIDConnection TestConnection(const std::string& from, const std::string& to, SocialConnectionType type) {
    ReddIDConnection connection;
    connection.fromReddId = from;
    connection.toReddId = to;
    connection.connectionType = type;
    connection.creationTime = GetTime();
    connection.lastInteraction = GetTime();
    return connection;
}

} // namespace reddid_test

/**
 * AuctionTestingSetup (clock stopped at T0, a namespace, shorthands for
 * auctions and bids) with shorthands for profiles, connections and
 * reputations.
 */
struct ProfileTestingSetup : public AuctionTestingSetup {
    ProfileManager* pMgr;

    ProfileTestingSetup() : pMgr(reddidManager->GetProfileManager()) {}

    /** Create a profile and require that it is accepted. */
    ReddIDProfile AddProfile(const ReddIDProfile& profile)
    {
        bool created = pMgr->CreateProfile(profile);
        assert(created);
        return profile;
    }

    ReddIDProfile AddProfile(const std::string& reddId, const CKeyID& owner)
    {
        return AddProfile(reddid_test::TestProfile(reddId, owner));
    }

    ReddIDProfile AddProfile(const std::string& reddId)
    {
        return AddProfile(reddId, MakeKeyID());
    }

    /** Create the profiles prefix0, prefix1, ... and return their names. */
    std::vector<std::string> AddProfiles(const std::string& prefix, int count)
    {
        std::vector<std::string> names;
        for (int i = 0; i < count; i++) {
            names.push_back(prefix + ToString(i));
            AddProfile(names.back());
        }
        return names;
    }

    /** Create or replace the connection from one profile to another. */
    void Connect(const std::string& from, const std::string& to, SocialConnectionType type)
    {
        bool connected = pMgr->CreateConnection(reddid_test::TestConnection(from, to, type));
        assert(connected);
    }

    /** Replace the stored reputation of a profile by one with the given overall score. */
    void SetOverallScore(const std::string& reddId, double score)
    {
        ReddIDReputation reputation;
        reputation.reddId = reddId;
        reputation.overallScore = score;
        bool updated = pMgr->UpdateReputation(reputation);
        assert(updated);
    }

    /**
     * Create the profiles prefix0, prefix1, ..., give each the overall score
     * and have each endorse the target. Returns their names.
     */
    std::vector<std::string> AddEndorsers(const std::string& prefix, int count, double score, const std::string& target)
    {
        std::vector<std::string> names = AddProfiles(prefix, count);
        for (const std::string& name : names) {
            SetOverallScore(name, score);
            Connect(name, target, CONNECTION_ENDORSE);
        }
        return names;
    }

    /** Calculate the reputation of a profile from the given auction statistics. */
    ReddIDReputation Calculate(const std::string& reddId, const BidderStats& stats = BidderStats())
    {
        ReddIDReputation reputation;
        bool calculated = pMgr->CalculateReputation(reddId, stats, reputation);
        assert(calculated);
        return reputation;
    }

    ReddIDProfile GetProfile(const std::string& reddId)
    {
        ReddIDProfile profile;
        bool found = pMgr->GetProfile(reddId, profile);
        assert(found);
        return profile;
    }

    ReddIDProfile GetStoredProfile(const std::string& reddId)
    {
        ReddIDProfile profile;
        bool found = db->ReadProfile(reddId, profile);
        assert(found);
        return profile;
    }
};

#endif // BITCOIN_TEST_REDDID_PROFILE_TEST_UTIL_H
