// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_REDDID_DB_H
#define BITCOIN_ID_REDDID_DB_H

#include <id/reddid.h>
#include <dbwrapper.h>
#include <uint256.h>

#include <memory>
#include <string>
#include <vector>

/**
 * Database schemas for ReddID system
 */

/**
 * Main ReddID database class
 */
class ReddIDDB : public CDBWrapper {
public:
    ReddIDDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
    
    // Namespace database operations
    bool WriteNamespace(const std::string& namespaceId, const NamespaceInfo& info);
    bool ReadNamespace(const std::string& namespaceId, NamespaceInfo& info);
    bool EraseNamespace(const std::string& namespaceId);
    bool ExistsNamespace(const std::string& namespaceId);
    bool ListNamespaces(std::vector<std::string>& namespaceIds);
    
    // Namespace pricing tiers
    bool WritePricingTier(const PricingTier& tier);
    bool ReadPricingTiers(const std::string& namespaceId, std::vector<PricingTier>& tiers);
    bool ErasePricingTier(const std::string& namespaceId, int minLength);
    
    // User ID database operations
    bool WriteUserID(const std::string& name, const std::string& namespaceId, const UserIDInfo& info);
    bool ReadUserID(const std::string& name, const std::string& namespaceId, UserIDInfo& info);
    bool EraseUserID(const std::string& name, const std::string& namespaceId);
    bool ExistsUserID(const std::string& name, const std::string& namespaceId);
    bool ListUserIDs(const std::string& namespaceId, std::vector<std::string>& names);
    bool ListUserIDsByOwner(const CKeyID& owner, std::vector<UserIDInfo>& ids);
    
    // Auction database operations
    bool WriteAuction(const uint256& auctionId, const AuctionInfo& info);
    bool ReadAuction(const uint256& auctionId, AuctionInfo& info);
    bool EraseAuction(const uint256& auctionId);
    bool ExistsAuction(const uint256& auctionId);
    bool ListAuctions(std::vector<uint256>& auctionIds);
    bool ListAuctionsByState(AuctionState state, std::vector<uint256>& auctionIds);
    bool ListAuctionsByNamespace(const std::string& namespaceId, std::vector<uint256>& auctionIds);
    
    // Bid database operations
    bool WriteBid(const uint256& bidId, const BidInfo& info);
    bool ReadBid(const uint256& bidId, BidInfo& info);
    bool EraseBid(const uint256& bidId);
    bool ExistsBid(const uint256& bidId);
    bool ListBids(const uint256& auctionId, std::vector<uint256>& bidIds);
    bool ListBidsByBidder(const CKeyID& bidder, std::vector<uint256>& bidIds);
    bool UpdateBidStatus(const uint256& bidId, bool isWinner, bool refunded);
    
    // ReddID profile database operations
    bool WriteProfile(const std::string& reddId, const ReddIDProfile& profile);
    bool ReadProfile(const std::string& reddId, ReddIDProfile& profile);
    bool EraseProfile(const std::string& reddId);
    bool ExistsProfile(const std::string& reddId);
    bool ListProfiles(std::vector<std::string>& reddIds);
    bool ListProfilesByOwner(const CKeyID& owner, std::vector<std::string>& reddIds);
    
    // Reputation database operations
    bool WriteReputation(const std::string& reddId, const ReddIDReputation& reputation);
    bool ReadReputation(const std::string& reddId, ReddIDReputation& reputation);
    bool EraseReputation(const std::string& reddId);
    bool ExistsReputation(const std::string& reddId);
    bool GetReputationHistory(const std::string& reddId, std::vector<ReddIDReputation>& history, int64_t startTime, int count);
    
    // Connection database operations
    bool WriteConnection(const std::string& fromReddId, const std::string& toReddId, 
                        const ReddIDConnection& connection);
    bool ReadConnection(const std::string& fromReddId, const std::string& toReddId, 
                       ReddIDConnection& connection);
    bool EraseConnection(const std::string& fromReddId, const std::string& toReddId);
    bool ExistsConnection(const std::string& fromReddId, const std::string& toReddId);
    bool ListConnections(const std::string& reddId, std::vector<ReddIDConnection>& connections);
    
    // Namespace resolution database operations
    bool WriteResolution(const std::string& reddId, const std::string& namespaceId, 
                        const std::string& userId);
    bool ReadResolution(const std::string& reddId, const std::string& namespaceId, 
                       std::string& userId);
    bool ReadResolutionByUserID(const std::string& userId, const std::string& namespaceId, 
                              std::string& reddId);
    bool EraseResolution(const std::string& reddId, const std::string& namespaceId);
    bool ListResolutions(const std::string& reddId, 
                       std::map<std::string, std::string>& resolutions);
};

#endif // BITCOIN_ID_REDDID_DB_H
