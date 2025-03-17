// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/reddid_db.h>

#include <chainparams.h>
#include <dbwrapper.h>
#include <fs.h>
#include <hash.h>
#include <logging.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <memory>
#include <string>
#include <vector>

ReddIDDB::ReddIDDB(size_t nCacheSize, bool fMemory, bool fWipe) 
    : CDBWrapper(gArgs.GetDataDirNet() / "reddid", nCacheSize, fMemory, fWipe) {
    LogPrintf("Initializing ReddID database in %s with cache size %u MB\n", 
             (gArgs.GetDataDirNet() / "reddid").string(), nCacheSize);
}

// Namespace database operations
bool ReddIDDB::WriteNamespace(const std::string& namespaceId, const NamespaceInfo& info) {
    std::string key = std::string("ns-") + namespaceId;
    return Write(key, info);
}

bool ReddIDDB::ReadNamespace(const std::string& namespaceId, NamespaceInfo& info) {
    std::string key = std::string("ns-") + namespaceId;
    return Read(key, info);
}

bool ReddIDDB::EraseNamespace(const std::string& namespaceId) {
    std::string key = std::string("ns-") + namespaceId;
    return Erase(key);
}

bool ReddIDDB::ExistsNamespace(const std::string& namespaceId) {
    std::string key = std::string("ns-") + namespaceId;
    return Exists(key);
}

bool ReddIDDB::ListNamespaces(std::vector<std::string>& namespaceIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "ns-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            std::string namespaceId = key.substr(prefix.size());
            namespaceIds.push_back(namespaceId);
        }
        
        pcursor->Next();
    }
    
    return true;
}

// Namespace pricing tiers
bool ReddIDDB::WritePricingTier(const PricingTier& tier) {
    std::string key = std::string("tier-") + tier.namespaceId + "-" + std::to_string(tier.minLength);
    return Write(key, tier);
}

bool ReddIDDB::ReadPricingTiers(const std::string& namespaceId, std::vector<PricingTier>& tiers) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "tier-" + namespaceId + "-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            PricingTier tier;
            if (pcursor->GetValue(tier)) {
                tiers.push_back(tier);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ErasePricingTier(const std::string& namespaceId, int minLength) {
    std::string key = std::string("tier-") + namespaceId + "-" + std::to_string(minLength);
    return Erase(key);
}

// User ID database operations
bool ReddIDDB::WriteUserID(const std::string& name, const std::string& namespaceId, const UserIDInfo& info) {
    std::string fullName = name + "." + namespaceId;
    std::string key = std::string("id-") + fullName;
    return Write(key, info);
}

bool ReddIDDB::ReadUserID(const std::string& name, const std::string& namespaceId, UserIDInfo& info) {
    std::string fullName = name + "." + namespaceId;
    std::string key = std::string("id-") + fullName;
    return Read(key, info);
}

bool ReddIDDB::EraseUserID(const std::string& name, const std::string& namespaceId) {
    std::string fullName = name + "." + namespaceId;
    std::string key = std::string("id-") + fullName;
    return Erase(key);
}

bool ReddIDDB::ExistsUserID(const std::string& name, const std::string& namespaceId) {
    std::string fullName = name + "." + namespaceId;
    std::string key = std::string("id-") + fullName;
    return Exists(key);
}

bool ReddIDDB::ListUserIDs(const std::string& namespaceId, std::vector<std::string>& names) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "id-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            std::string fullName = key.substr(prefix.size());
            
            // Parse name and namespace
            size_t pos = fullName.find_last_of('.');
            if (pos != std::string::npos) {
                std::string name = fullName.substr(0, pos);
                std::string ns = fullName.substr(pos + 1);
                
                if (namespaceId.empty() || ns == namespaceId) {
                    names.push_back(name);
                }
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ListUserIDsByOwner(const CKeyID& owner, std::vector<UserIDInfo>& ids) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "id-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            UserIDInfo info;
            if (pcursor->GetValue(info) && info.owner == owner) {
                ids.push_back(info);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

// Auction database operations
bool ReddIDDB::WriteAuction(const uint256& auctionId, const AuctionInfo& info) {
    std::string key = std::string("auc-") + auctionId.ToString();
    return Write(key, info);
}

bool ReddIDDB::ReadAuction(const uint256& auctionId, AuctionInfo& info) {
    std::string key = std::string("auc-") + auctionId.ToString();
    return Read(key, info);
}

bool ReddIDDB::EraseAuction(const uint256& auctionId) {
    std::string key = std::string("auc-") + auctionId.ToString();
    return Erase(key);
}

bool ReddIDDB::ExistsAuction(const uint256& auctionId) {
    std::string key = std::string("auc-") + auctionId.ToString();
    return Exists(key);
}

bool ReddIDDB::ListAuctions(std::vector<uint256>& auctionIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "auc-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            std::string auctionIdStr = key.substr(prefix.size());
            uint256 auctionId;
            auctionId.SetHex(auctionIdStr);
            auctionIds.push_back(auctionId);
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ListAuctionsByState(AuctionState state, std::vector<uint256>& auctionIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "auc-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            AuctionInfo info;
            if (pcursor->GetValue(info) && info.state == state) {
                auctionIds.push_back(info.auctionId);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ListAuctionsByNamespace(const std::string& namespaceId, std::vector<uint256>& auctionIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "auc-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            AuctionInfo info;
            if (pcursor->GetValue(info) && info.namespaceId == namespaceId) {
                auctionIds.push_back(info.auctionId);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

// Bid database operations
bool ReddIDDB::WriteBid(const uint256& bidId, const BidInfo& info) {
    std::string key = std::string("bid-") + bidId.ToString();
    return Write(key, info);
}

bool ReddIDDB::ReadBid(const uint256& bidId, BidInfo& info) {
    std::string key = std::string("bid-") + bidId.ToString();
    return Read(key, info);
}

bool ReddIDDB::EraseBid(const uint256& bidId) {
    std::string key = std::string("bid-") + bidId.ToString();
    return Erase(key);
}

bool ReddIDDB::ExistsBid(const uint256& bidId) {
    std::string key = std::string("bid-") + bidId.ToString();
    return Exists(key);
}

bool ReddIDDB::ListBids(const uint256& auctionId, std::vector<uint256>& bidIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "bid-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            BidInfo info;
            if (pcursor->GetValue(info) && info.auctionId == auctionId) {
                bidIds.push_back(info.bidId);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ListBidsByBidder(const CKeyID& bidder, std::vector<uint256>& bidIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "bid-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            BidInfo info;
            if (pcursor->GetValue(info) && info.bidder == bidder) {
                bidIds.push_back(info.bidId);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::UpdateBidStatus(const uint256& bidId, bool isWinner, bool refunded) {
    BidInfo info;
    if (!ReadBid(bidId, info)) {
        return false;
    }
    
    info.isWinner = isWinner;
    info.refunded = refunded;
    
    return WriteBid(bidId, info);
}

// ReddID profile database operations
bool ReddIDDB::WriteProfile(const std::string& reddId, const ReddIDProfile& profile) {
    std::string key = std::string("profile-") + reddId;
    return Write(key, profile);
}

bool ReddIDDB::ReadProfile(const std::string& reddId, ReddIDProfile& profile) {
    std::string key = std::string("profile-") + reddId;
    return Read(key, profile);
}

bool ReddIDDB::EraseProfile(const std::string& reddId) {
    std::string key = std::string("profile-") + reddId;
    return Erase(key);
}

bool ReddIDDB::ExistsProfile(const std::string& reddId) {
    std::string key = std::string("profile-") + reddId;
    return Exists(key);
}

bool ReddIDDB::ListProfiles(std::vector<std::string>& reddIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "profile-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            std::string reddId = key.substr(prefix.size());
            reddIds.push_back(reddId);
        }
        
        pcursor->Next();
    }
    
    return true;
}

bool ReddIDDB::ListProfilesByOwner(const CKeyID& owner, std::vector<std::string>& reddIds) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "profile-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            ReddIDProfile profile;
            if (pcursor->GetValue(profile) && profile.owner == owner) {
                std::string reddId = key.substr(prefix.size());
                reddIds.push_back(reddId);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

// Reputation database operations
bool ReddIDDB::WriteReputation(const std::string& reddId, const ReddIDReputation& reputation) {
    std::string key = std::string("rep-") + reddId;
    return Write(key, reputation);
}

bool ReddIDDB::ReadReputation(const std::string& reddId, ReddIDReputation& reputation) {
    std::string key = std::string("rep-") + reddId;
    return Read(key, reputation);
}

bool ReddIDDB::EraseReputation(const std::string& reddId) {
    std::string key = std::string("rep-") + reddId;
    return Erase(key);
}

bool ReddIDDB::ExistsReputation(const std::string& reddId) {
    std::string key = std::string("rep-") + reddId;
    return Exists(key);
}

bool ReddIDDB::GetReputationHistory(const std::string& reddId, std::vector<ReddIDReputation>& history,
                         int64_t startTime = 0, int count = 10) {
    // For now, we don't have actual history storage, so we'll mock this
    // In a real implementation, this would query a history table in the database

    // We'll just return the current reputation as historical entry
    ReddIDReputation currentRep;
    if (ReadReputation(reddId, currentRep)) {
        // Only include if it's before the startTime
        if (currentRep.lastCalculated <= startTime) {
            history.push_back(currentRep);
        }
    }

    return !history.empty();
}

// Connection database operations
bool ReddIDDB::WriteConnection(const std::string& fromReddId, const std::string& toReddId, 
                              const ReddIDConnection& connection) {
    std::string key = std::string("conn-") + fromReddId + "-" + toReddId;
    return Write(key, connection);
}

bool ReddIDDB::ReadConnection(const std::string& fromReddId, const std::string& toReddId, 
                             ReddIDConnection& connection) {
    std::string key = std::string("conn-") + fromReddId + "-" + toReddId;
    return Read(key, connection);
}

bool ReddIDDB::EraseConnection(const std::string& fromReddId, const std::string& toReddId) {
    std::string key = std::string("conn-") + fromReddId + "-" + toReddId;
    return Erase(key);
}

bool ReddIDDB::ExistsConnection(const std::string& fromReddId, const std::string& toReddId) {
    std::string key = std::string("conn-") + fromReddId + "-" + toReddId;
    return Exists(key);
}

bool ReddIDDB::ListConnections(const std::string& reddId, std::vector<ReddIDConnection>& connections) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "conn-" + reddId + "-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            ReddIDConnection connection;
            if (pcursor->GetValue(connection)) {
                connections.push_back(connection);
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}

// Namespace resolution database operations
bool ReddIDDB::WriteResolution(const std::string& reddId, const std::string& namespaceId, 
                              const std::string& userId) {
    std::string key = std::string("res-reddid-") + reddId + "-" + namespaceId;
    bool result = Write(key, userId);
    
    if (result) {
        // Write reverse lookup
        std::string reverseKey = std::string("res-userid-") + userId + "-" + namespaceId;
        result = Write(reverseKey, reddId);
    }
    
    return result;
}

bool ReddIDDB::ReadResolution(const std::string& reddId, const std::string& namespaceId, 
                             std::string& userId) {
    std::string key = std::string("res-reddid-") + reddId + "-" + namespaceId;
    return Read(key, userId);
}

bool ReddIDDB::ReadResolutionByUserID(const std::string& userId, const std::string& namespaceId, 
                                    std::string& reddId) {
    std::string key = std::string("res-userid-") + userId + "-" + namespaceId;
    return Read(key, reddId);
}

bool ReddIDDB::EraseResolution(const std::string& reddId, const std::string& namespaceId) {
    // Get current user ID
    std::string userId;
    if (ReadResolution(reddId, namespaceId, userId)) {
        // Erase reverse lookup
        std::string reverseKey = std::string("res-userid-") + userId + "-" + namespaceId;
        Erase(reverseKey);
    }
    
    // Erase forward lookup
    std::string key = std::string("res-reddid-") + reddId + "-" + namespaceId;
    return Erase(key);
}

bool ReddIDDB::ListResolutions(const std::string& reddId, 
                             std::map<std::string, std::string>& resolutions) {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    
    std::string prefix = "res-reddid-" + reddId + "-";
    
    pcursor->Seek(prefix);
    
    while (pcursor->Valid()) {
        std::string key;
        if (pcursor->GetKey(key) && key.find(prefix) == 0) {
            std::string namespaceId = key.substr(prefix.size());
            std::string userId;
            
            if (pcursor->GetValue(userId)) {
                resolutions[namespaceId] = userId;
            }
        }
        
        pcursor->Next();
    }
    
    return true;
}
