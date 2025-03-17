// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/namespace.h>
#include <id/reddid_db.h>
#include <id/reddid_p2p.h>

#include <key.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <util/system.h>
#include <util/strencodings.h>

#include <algorithm>
#include <regex>
#include <map>
#include <string>
#include <vector>

NamespaceManager::NamespaceManager(ReddIDManager& manager)
    : reddIDManager(&manager) {
    // Initialize with ReddIDManager references
    reddidDB = manager.GetReddIDDB();
    reddidP2P = manager.GetP2PManager();
}

NamespaceManager::~NamespaceManager() {
    Save();
}

bool NamespaceManager::Load() {
    LogPrintf("Loading namespace data...\n");
    
    // Clear existing data
    namespaces.clear();
    namespaceAuctions.clear();
    namespaceAuctionBids.clear();
    namespacePricingTiers.clear();
    
    // Load namespaces
    std::vector<std::string> namespaceIds;
    if (!reddidDB->ListNamespaces(namespaceIds)) {
        LogPrintf("Error: Failed to list namespaces\n");
        return false;
    }
    
    for (const auto& nsId : namespaceIds) {
        NamespaceInfo info;
        if (reddidDB->ReadNamespace(nsId, info)) {
            namespaces[nsId] = info;
            
            // Load pricing tiers for this namespace
            std::vector<PricingTier> tiers;
            if (reddidDB->ReadPricingTiers(nsId, tiers)) {
                namespacePricingTiers[nsId] = tiers;
            }
        }
    }
    
    // Load namespace auctions
    std::vector<uint256> auctionIds;
    if (!reddidDB->ListAuctions(auctionIds)) {
        LogPrintf("Error: Failed to list namespace auctions\n");
        return false;
    }
    
    for (const auto& auctionId : auctionIds) {
        AuctionInfo auction;
        if (reddidDB->ReadAuction(auctionId, auction)) {
            namespaceAuctions[auctionId] = auction;
            
            // Load bids for this auction
            std::vector<uint256> bidIds;
            if (reddidDB->ListBids(auctionId, bidIds)) {
                std::vector<BidInfo> bids;
                for (const auto& bidId : bidIds) {
                    BidInfo bid;
                    if (reddidDB->ReadBid(bidId, bid)) {
                        bids.push_back(bid);
                    }
                }
                namespaceAuctionBids[auctionId] = bids;
            }
        }
    }
    
    LogPrintf("Loaded %d namespaces and %d namespace auctions\n", 
             namespaces.size(), namespaceAuctions.size());
    
    return true;
}

bool NamespaceManager::Save() const {
    LogPrintf("Saving namespace data...\n");
    
    // Save namespaces
    for (const auto& pair : namespaces) {
        if (!reddidDB->WriteNamespace(pair.first, pair.second)) {
            LogPrintf("Error: Failed to save namespace %s\n", pair.first);
            return false;
        }
        
        // Save pricing tiers
        auto it = namespacePricingTiers.find(pair.first);
        if (it != namespacePricingTiers.end()) {
            for (const auto& tier : it->second) {
                if (!reddidDB->WritePricingTier(tier)) {
                    LogPrintf("Error: Failed to save pricing tier for namespace %s\n", pair.first);
                    return false;
                }
            }
        }
    }
    
    // Save auctions and bids
    for (const auto& pair : namespaceAuctions) {
        if (!reddidDB->WriteAuction(pair.first, pair.second)) {
            LogPrintf("Error: Failed to save namespace auction %s\n", pair.first.ToString());
            return false;
        }
        
        // Save bids
        auto it = namespaceAuctionBids.find(pair.first);
        if (it != namespaceAuctionBids.end()) {
            for (const auto& bid : it->second) {
                if (!reddidDB->WriteBid(bid.bidId, bid)) {
                    LogPrintf("Error: Failed to save bid %s\n", bid.bidId.ToString());
                    return false;
                }
            }
        }
    }
    
    return true;
}

bool NamespaceManager::CreateNamespace(const NamespaceInfo& namespaceInfo) {
    // Check if namespace already exists
    if (namespaces.find(namespaceInfo.id) != namespaces.end()) {
        return false;
    }
    
    // Validate namespace configuration
    if (!ValidateNamespaceConfig(namespaceInfo)) {
        return false;
    }
    
    // Calculate config hash
    NamespaceInfo nsInfo = namespaceInfo;
    nsInfo.configHash = CalculateConfigHash(nsInfo);
    nsInfo.lastUpdated = GetTime();
    
    // Set expiration time if not already set
    if (nsInfo.expiration == 0) {
        nsInfo.expiration = nsInfo.lastUpdated + (nsInfo.renewalPeriod * 24 * 60 * 60);
    }
    
    // Add to memory
    namespaces[nsInfo.id] = nsInfo;
    
    // Save to database
    return reddidDB->WriteNamespace(nsInfo.id, nsInfo);
}

bool NamespaceManager::UpdateNamespace(const NamespaceInfo& namespaceInfo) {
    // Check if namespace exists
    auto it = namespaces.find(namespaceInfo.id);
    if (it == namespaces.end()) {
        return false;
    }
    
    // Check if owner matches
    if (it->second.owner != namespaceInfo.owner) {
        return false;
    }
    
    // Validate namespace configuration
    if (!ValidateNamespaceConfig(namespaceInfo)) {
        return false;
    }
    
    // Calculate config hash
    NamespaceInfo nsInfo = namespaceInfo;
    nsInfo.configHash = CalculateConfigHash(nsInfo);
    nsInfo.lastUpdated = GetTime();
    
    // Preserve expiration time
    nsInfo.expiration = it->second.expiration;
    
    // Update in memory
    namespaces[nsInfo.id] = nsInfo;
    
    // Save to database
    return reddidDB->WriteNamespace(nsInfo.id, nsInfo);
}

bool NamespaceManager::RenewNamespace(const std::string& namespaceId, const CKeyID& owner, int renewalDays) {
    // Check if namespace exists
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return false;
    }
    
    // Check if owner matches
    if (it->second.owner != owner) {
        return false;
    }
    
    // Calculate renewal fee
    CAmount renewalFee = CalculateRenewalFee(namespaceId);
    
    // Update expiration time
    int64_t currentTime = GetTime();
    if (it->second.expiration > currentTime) {
        // If not expired, add renewal period to current expiration
        it->second.expiration += renewalDays * 24 * 60 * 60;
    } else {
        // If expired, set new expiration from current time
        it->second.expiration = currentTime + renewalDays * 24 * 60 * 60;
    }
    
    // Save to database
    return reddidDB->WriteNamespace(namespaceId, it->second);
}

bool NamespaceManager::TransferNamespace(const std::string& namespaceId, const CKeyID& from, const CKeyID& to) {
    // Check if namespace exists
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return false;
    }
    
    // Check if current owner matches
    if (it->second.owner != from) {
        return false;
    }
    
    // Update owner
    it->second.owner = to;
    it->second.lastUpdated = GetTime();
    
    // Save to database
    return reddidDB->WriteNamespace(namespaceId, it->second);
}

bool NamespaceManager::ExpireNamespace(const std::string& namespaceId) {
    // Check if namespace exists
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return false;
    }
    
    // Check if namespace has expired
    int64_t currentTime = GetTime();
    if (it->second.expiration + (it->second.gracePeriod * 24 * 60 * 60) > currentTime) {
        // Not expired yet
        return false;
    }
    
    // Remove namespace
    namespaces.erase(it);
    
    // Remove from database
    return reddidDB->EraseNamespace(namespaceId);
}

bool NamespaceManager::CreateNamespaceAuction(const std::string& namespaceId, const CKeyID& creator,
                                            CAmount reservePrice, int durationDays, AuctionType type,
                                            uint256& auctionId) {
    // Validate namespace ID
    if (!ValidateNamespaceID(namespaceId)) {
        return false;
    }
    
    // Check if namespace is available
    if (!IsNamespaceAvailable(namespaceId)) {
        return false;
    }
    
    // Check if reserve price meets minimum
    CAmount minPrice = CalculateMinPrice(namespaceId);
    if (reservePrice < minPrice) {
        return false;
    }
    
    // Create auction
    AuctionInfo auction;
    auction.name = ""; // No name for namespace auctions
    auction.namespaceId = namespaceId;
    auction.creator = creator;
    auction.startTime = GetTime();
    auction.endTime = auction.startTime + (durationDays * 24 * 60 * 60);
    auction.reservePrice = reservePrice;
    auction.currentBid = 0;
    auction.depositAmount = reservePrice * 0.2; // 20% deposit for namespaces
    auction.state = AUCTION_PENDING;
    auction.type = type;
    
    // Generate auction ID
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << auction.namespaceId;
    ss << auction.startTime;
    auctionId = Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
    auction.auctionId = auctionId;
    
    // Save auction
    namespaceAuctions[auctionId] = auction;
    namespaceAuctionBids[auctionId] = std::vector<BidInfo>();
    
    // Save to database
    if (!reddidDB->WriteAuction(auctionId, auction)) {
        namespaceAuctions.erase(auctionId);
        namespaceAuctionBids.erase(auctionId);
        return false;
    }
    
    // Announce auction via P2P
    if (reddidP2P != nullptr) {
        reddidP2P->AnnounceNamespaceAuction(auction);
    } else {
        LogPrintf("Warning: P2P manager not available, auction will not be announced\n");
    }
    
    return true;
}

bool NamespaceManager::BidOnNamespaceAuction(const uint256& auctionId, const CKeyID& bidder, 
                                           CAmount bidAmount, uint256& bidId) {
    // Check if auction exists
    auto auctionIt = namespaceAuctions.find(auctionId);
    if (auctionIt == namespaceAuctions.end()) {
        return false;
    }
    
    AuctionInfo& auction = auctionIt->second;
    
    // Check if auction is active
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        return false;
    }
    
    // If auction is pending, activate it
    if (auction.state == AUCTION_PENDING) {
        auction.state = AUCTION_ACTIVE;
        reddidDB->WriteAuction(auctionId, auction);
    }
    
    // Check if bid meets minimum
    if (bidAmount < auction.reservePrice) {
        return false;
    }
    
    // Check if bid is higher than current bid
    if (auction.currentBid > 0) {
        // Calculate minimum increment (5%)
        CAmount minIncrement = auction.currentBid * 0.05;
        if (bidAmount < auction.currentBid + minIncrement) {
            return false;
        }
    }
    
    // Create bid
    BidInfo bid;
    bid.auctionId = auctionId;
    bid.bidder = bidder;
    bid.bidAmount = bidAmount;
    bid.depositAmount = auction.depositAmount;
    bid.bidTime = GetTime();
    bid.isWinner = false;
    bid.refunded = false;
    
    // Generate bid ID
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << auctionId;
    ss << bidder;
    ss << bidAmount;
    ss << bid.bidTime;

    bidId = Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
    bid.bidId = bidId;
    
    // Update auction
    auction.currentBid = bidAmount;
    auction.currentBidder = bidder;
    
    // Check if we need to extend the auction (anti-sniping)
    int64_t currentTime = GetTime();
    int64_t timeLeft = auction.endTime - currentTime;
    if (timeLeft < 24 * 60 * 60) { // Less than 24 hours left
        // Extend by 12 hours
        auction.endTime = currentTime + 12 * 60 * 60;
        LogPrintf("Extending namespace auction %s by 12 hours due to late bid\n", auctionId.ToString());
    }
    
    // Save bid and updated auction
    namespaceAuctionBids[auctionId].push_back(bid);
    
    if (!reddidDB->WriteBid(bidId, bid)) {
        return false;
    }
    
    if (!reddidDB->WriteAuction(auctionId, auction)) {
        return false;
    }
    
    // Announce bid via P2P
    if (reddidP2P != nullptr) {
        reddidP2P->AnnounceNamespaceBid(bid);
    } else {
        LogPrintf("Warning: P2P manager not available, auction will not be announced\n");
    }
    
    return true;
}

bool NamespaceManager::FinalizeNamespaceAuction(const uint256& auctionId) {
    // Check if auction exists
    auto auctionIt = namespaceAuctions.find(auctionId);
    if (auctionIt == namespaceAuctions.end()) {
        return false;
    }
    
    AuctionInfo& auction = auctionIt->second;
    
    // Check if auction is active
    if (auction.state != AUCTION_ACTIVE) {
        return false;
    }
    
    // Check if auction has ended
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime) {
        return false;
    }
    
    // Mark auction as ended
    auction.state = AUCTION_ENDED;
    reddidDB->WriteAuction(auctionId, auction);
    
    // Check if there were any bids
    if (auction.currentBid == 0) {
        // No bids, cancel auction
        auction.state = AUCTION_CANCELED;
        reddidDB->WriteAuction(auctionId, auction);
        return true;
    }
    
    // Get highest bid
    auto bidsIt = namespaceAuctionBids.find(auctionId);
    if (bidsIt == namespaceAuctionBids.end() || bidsIt->second.empty()) {
        return false;
    }
    
    // Find winning bid
    BidInfo winningBid;
    CAmount highestBid = 0;
    for (const auto& bid : bidsIt->second) {
        if (bid.bidAmount > highestBid) {
            highestBid = bid.bidAmount;
            winningBid = bid;
        }
    }
    
    // Mark winning bid
    for (auto& bid : bidsIt->second) {
        if (bid.bidId == winningBid.bidId) {
            bid.isWinner = true;
            reddidDB->UpdateBidStatus(bid.bidId, true, false);
        }
    }
    
    // Create namespace
    NamespaceInfo nsInfo;
    nsInfo.id = auction.namespaceId;
    nsInfo.owner = winningBid.bidder;
    nsInfo.allowNumbers = true;
    nsInfo.allowHyphens = true;
    nsInfo.allowUnderscores = true;
    nsInfo.minLength = 3;
    nsInfo.maxLength = 32;
    nsInfo.renewalPeriod = 365;
    nsInfo.gracePeriod = 30;
    nsInfo.namespaceRevenuePct = 10;
    nsInfo.burnPct = 70;
    nsInfo.nodePct = 15;
    nsInfo.devPct = 5;
    nsInfo.minAuctionDuration = 3;
    nsInfo.maxAuctionDuration = 7;
    nsInfo.minBidIncrement = 5.0;
    nsInfo.lastUpdated = currentTime;
    nsInfo.expiration = currentTime + (nsInfo.renewalPeriod * 24 * 60 * 60);
    nsInfo.configHash = CalculateConfigHash(nsInfo);
    
    // Add default pricing tiers
    std::vector<PricingTier> tiers;
    
    PricingTier tier1;
    tier1.namespaceId = auction.namespaceId;
    tier1.minLength = 1;
    tier1.minPrice = 100000 * COIN; // 100,000 RDD for single character
    tiers.push_back(tier1);
    
    PricingTier tier2;
    tier2.namespaceId = auction.namespaceId;
    tier2.minLength = 2;
    tier2.minPrice = 50000 * COIN; // 50,000 RDD for 2 characters
    tiers.push_back(tier2);
    
    PricingTier tier3;
    tier3.namespaceId = auction.namespaceId;
    tier3.minLength = 3;
    tier3.minPrice = 25000 * COIN; // 25,000 RDD for 3 characters
    tiers.push_back(tier3);
    
    PricingTier tier4;
    tier4.namespaceId = auction.namespaceId;
    tier4.minLength = 4;
    tier4.minPrice = 10000 * COIN; // 10,000 RDD for 4+ characters
    tiers.push_back(tier4);
    
    // Save namespace and pricing tiers
    if (!CreateNamespace(nsInfo)) {
        return false;
    }
    
    namespacePricingTiers[auction.namespaceId] = tiers;
    for (const auto& tier : tiers) {
        reddidDB->WritePricingTier(tier);
    }
    
    // Distribute auction proceeds
    DistributeNamespaceAuctionProceeds(auction, winningBid);
    
    // Refund losing bids
    for (auto& bid : bidsIt->second) {
        if (!bid.isWinner && !bid.refunded) {
            // Refund would happen in a real implementation
            // For now, just mark as refunded
            bid.refunded = true;
            reddidDB->UpdateBidStatus(bid.bidId, false, true);
        }
    }
    
    // Mark auction as finalized
    auction.state = AUCTION_FINALIZED;
    reddidDB->WriteAuction(auctionId, auction);
    
    // Announce finalization via P2P
    if (reddidP2P != nullptr) {
        reddidP2P->AnnounceNamespaceFinalize(auctionId, winningBid.bidId, winningBid.bidAmount);
    } else {
        LogPrintf("Warning: P2P manager not available, auction will not be announced\n");
    }
    
    return true;
}

bool NamespaceManager::CancelNamespaceAuction(const uint256& auctionId, const CKeyID& creator) {
    // Check if auction exists
    auto auctionIt = namespaceAuctions.find(auctionId);
    if (auctionIt == namespaceAuctions.end()) {
        return false;
    }
    
    AuctionInfo& auction = auctionIt->second;
    
    // Check if creator matches
    if (auction.creator != creator) {
        return false;
    }
    
    // Check if auction can be canceled (only pending or no bids)
    if (auction.state != AUCTION_PENDING && auction.currentBid > 0) {
        return false;
    }
    
    // Mark auction as canceled
    auction.state = AUCTION_CANCELED;
    reddidDB->WriteAuction(auctionId, auction);
    
    // Announce cancellation via P2P
    if (reddidP2P != nullptr) {
        reddidP2P->AnnounceNamespaceCancel(auctionId);
    } else {
        LogPrintf("Warning: P2P manager not available, auction will not be announced\n");
    }
    
    return true;
}

bool NamespaceManager::AddPricingTier(const std::string& namespaceId, int minLength, CAmount minPrice) {
    // Check if namespace exists
    if (namespaces.find(namespaceId) == namespaces.end()) {
        return false;
    }
    
    // Create pricing tier
    PricingTier tier;
    tier.namespaceId = namespaceId;
    tier.minLength = minLength;
    tier.minPrice = minPrice;
    
    // Add to memory
    auto& tiers = namespacePricingTiers[namespaceId];
    
    // Check if tier with this min length already exists
    for (size_t i = 0; i < tiers.size(); i++) {
        if (tiers[i].minLength == minLength) {
            // Update existing tier
            tiers[i].minPrice = minPrice;
            return reddidDB->WritePricingTier(tiers[i]);
        }
    }
    
    // Add new tier
    tiers.push_back(tier);
    
    // Save to database
    return reddidDB->WritePricingTier(tier);
}

bool NamespaceManager::UpdatePricingTier(const std::string& namespaceId, int minLength, CAmount minPrice) {
    // Check if namespace exists
    if (namespaces.find(namespaceId) == namespaces.end()) {
        return false;
    }
    
    // Check if tier exists
    auto it = namespacePricingTiers.find(namespaceId);
    if (it == namespacePricingTiers.end()) {
        return false;
    }
    
    // Find tier with this min length
    for (auto& tier : it->second) {
        if (tier.minLength == minLength) {
            // Update tier
            tier.minPrice = minPrice;
            
            // Save to database
            return reddidDB->WritePricingTier(tier);
        }
    }
    
    // Tier not found
    return false;
}

bool NamespaceManager::RemovePricingTier(const std::string& namespaceId, int minLength) {
    // Check if namespace exists
    if (namespaces.find(namespaceId) == namespaces.end()) {
        return false;
    }
    
    // Check if tier exists
    auto it = namespacePricingTiers.find(namespaceId);
    if (it == namespacePricingTiers.end()) {
        return false;
    }
    
    // Find tier with this min length
    for (auto tierIt = it->second.begin(); tierIt != it->second.end(); ++tierIt) {
        if (tierIt->minLength == minLength) {
            // Remove tier
            it->second.erase(tierIt);
            
            // Remove from database
            return reddidDB->ErasePricingTier(namespaceId, minLength);
        }
    }
    
    // Tier not found
    return false;
}

bool NamespaceManager::IsNamespaceAvailable(const std::string& namespaceId) const {
    // Check if namespace exists in memory
    if (namespaces.find(namespaceId) != namespaces.end()) {
        return false;
    }
    
    // Check if namespace exists in database
    return !reddidDB->ExistsNamespace(namespaceId);
}

bool NamespaceManager::IsNamespaceValid(const std::string& namespaceId) const {
    return ValidateNamespaceID(namespaceId);
}

bool NamespaceManager::IsOwner(const std::string& namespaceId, const CKeyID& keyId) const {
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return false;
    }
    
    return it->second.owner == keyId;
}

bool NamespaceManager::HasExpired(const std::string& namespaceId) const {
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return false;
    }
    
    int64_t currentTime = GetTime();
    int64_t expirationWithGrace = it->second.expiration + (it->second.gracePeriod * 24 * 60 * 60);
    
    return currentTime > expirationWithGrace;
}

bool NamespaceManager::GetNamespaceInfo(const std::string& namespaceId, NamespaceInfo& result) const {
    auto it = namespaces.find(namespaceId);
    if (it == namespaces.end()) {
        return reddidDB->ReadNamespace(namespaceId, result);
    }
    
    result = it->second;
    return true;
}

bool NamespaceManager::GetAuctionInfo(const uint256& auctionId, AuctionInfo& result) const {
    auto it = namespaceAuctions.find(auctionId);
    if (it == namespaceAuctions.end()) {
        return reddidDB->ReadAuction(auctionId, result);
    }
    
    result = it->second;
    return true;
}

std::vector<AuctionInfo> NamespaceManager::GetActiveAuctions() const {
    std::vector<AuctionInfo> result;
    
    for (const auto& pair : namespaceAuctions) {
        if (pair.second.state == AUCTION_ACTIVE) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

std::vector<BidInfo> NamespaceManager::GetAuctionBids(const uint256& auctionId) const {
    auto it = namespaceAuctionBids.find(auctionId);
    if (it == namespaceAuctionBids.end()) {
        std::vector<BidInfo> result;
        std::vector<uint256> bidIds;
        
        if (reddidDB->ListBids(auctionId, bidIds)) {
            for (const auto& bidId : bidIds) {
                BidInfo bid;
                if (reddidDB->ReadBid(bidId, bid)) {
                    result.push_back(bid);
                }
            }
        }
        
        return result;
    }
    
    return it->second;
}

std::vector<NamespaceInfo> NamespaceManager::GetNamespaces() const {
    std::vector<NamespaceInfo> result;
    
    for (const auto& pair : namespaces) {
        result.push_back(pair.second);
    }
    
    return result;
}

std::vector<PricingTier> NamespaceManager::GetPricingTiers(const std::string& namespaceId) const {
    auto it = namespacePricingTiers.find(namespaceId);
    if (it == namespacePricingTiers.end()) {
        std::vector<PricingTier> result;
        reddidDB->ReadPricingTiers(namespaceId, result);
        return result;
    }
    
    return it->second;
}

CAmount NamespaceManager::CalculateMinPrice(const std::string& namespaceId) const {
    CAmount basePrice = NAMESPACE_MIN_FEE;
    
    // Length-based pricing
    size_t length = namespaceId.size();
    
    if (length == 1) {
        return 100000 * COIN; // 100,000 RDD for single character
    } else if (length == 2) {
        return 50000 * COIN; // 50,000 RDD for 2 characters
    } else if (length == 3) {
        return 25000 * COIN; // 25,000 RDD for 3 characters
    } else {
        return 10000 * COIN; // 10,000 RDD for 4+ characters
    }
}

CAmount NamespaceManager::CalculateRenewalFee(const std::string& namespaceId) const {
    // Base renewal fee is 25% of equivalent auction price
    CAmount auctionPrice = CalculateMinPrice(namespaceId);
    return auctionPrice * 0.25;
}

bool NamespaceManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    // Process namespace auction transactions
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        
        // Check if this is an OP_RETURN output
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
            // Extract data from OP_RETURN
            std::vector<unsigned char> vchData;
            if (txout.scriptPubKey.size() > 1) {
                opcodetype opcode;
                CScript::const_iterator pc = txout.scriptPubKey.begin() + 1;
                if (txout.scriptPubKey.GetOp(pc, opcode, vchData) && vchData.size() > 2 && vchData[0] == 'R') {
                    unsigned char opCode = vchData[1];
                    
                    // Process based on operation code
                    if (opCode == OP_NAMESPACE_AUCTION_CREATE) {
                        // Parse namespace auction creation
                        // In a real implementation, we would extract the namespace ID and other parameters
                        // from the OP_RETURN data and create an auction
                        
                        // For now, just log that we received a namespace auction creation
                        LogPrintf("Received namespace auction creation transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_NAMESPACE_AUCTION_BID) {
                        // Parse namespace auction bid
                        LogPrintf("Received namespace auction bid transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_NAMESPACE_AUCTION_FINALIZE) {
                        // Parse namespace auction finalization
                        LogPrintf("Received namespace auction finalization transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_NAMESPACE_AUCTION_CANCEL) {
                        // Parse namespace auction cancellation
                        LogPrintf("Received namespace auction cancellation transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

bool NamespaceManager::ValidateNamespaceID(const std::string& namespaceId) const {
    // Check length
    if (namespaceId.size() < MIN_NAMESPACE_LENGTH || namespaceId.size() > MAX_NAMESPACE_LENGTH) {
        return false;
    }
    
    // Use regex to validate format (lowercase letters, numbers, hyphens, not starting or ending with hyphen)
    std::regex pattern("^[a-z0-9]+(-[a-z0-9]+)*$");
    return std::regex_match(namespaceId, pattern);
}

bool NamespaceManager::ValidateNamespaceConfig(const NamespaceInfo& config) {
    // Validate ID
    if (!ValidateNamespaceID(config.id)) {
        return false;
    }
    
    // Validate required percentages
    int totalPct = config.namespaceRevenuePct + config.burnPct + config.nodePct + config.devPct;
    if (totalPct != 100) {
        return false;
    }
    
    // Validate percentage constraints
    if (config.burnPct < 50 || config.burnPct > 80) {
        return false; // Burn rate must be 50-80%
    }
    
    if (config.namespaceRevenuePct < 5 || config.namespaceRevenuePct > 10) {
        return false; // Namespace owner revenue must be 5-10%
    }
    
    if (config.nodePct < 5 || config.nodePct > 25) {
        return false; // Node operators share must be 5-25%
    }
    
    if (config.devPct < 5 || config.devPct > 15) {
        return false; // Development fund must be 5-15%
    }
    
    // Validate length requirements
    if (config.minLength < 1 || config.minLength > 10) {
        return false;
    }
    
    if (config.maxLength < 5 || config.maxLength > 64) {
        return false;
    }
    
    if (config.minLength > config.maxLength) {
        return false;
    }
    
    // Validate time periods
    if (config.renewalPeriod < 180 || config.renewalPeriod > 730) {
        return false; // 180-730 days (6 months to 2 years)
    }
    
    if (config.gracePeriod < 14 || config.gracePeriod > 60) {
        return false; // 14-60 days
    }
    
    // Validate auction parameters
    if (config.minAuctionDuration < 1 || config.minAuctionDuration > 7) {
        return false;
    }
    
    if (config.maxAuctionDuration < 3 || config.maxAuctionDuration > 14) {
        return false;
    }
    
    if (config.minAuctionDuration > config.maxAuctionDuration) {
        return false;
    }
    
    if (config.minBidIncrement < 1.0 || config.minBidIncrement > 10.0) {
        return false;
    }
    
    return true;
}

bool NamespaceManager::ValidateNamespaceCharacters(const std::string& namespaceId) {
    for (const char& c : namespaceId) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    
    // Cannot begin or end with a hyphen
    if (namespaceId[0] == '-' || namespaceId[namespaceId.size() - 1] == '-') {
        return false;
    }
    
    return true;
}

uint256 NamespaceManager::CalculateConfigHash(const NamespaceInfo& config) {
    // Serialize config to calculate hash
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << config;
    
    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

void NamespaceManager::DistributeNamespaceAuctionProceeds(const AuctionInfo& auction, const BidInfo& winningBid) {
    // In a real implementation, this would distribute the auction proceeds
    // For now, just log the distribution
    LogPrintf("Distributing namespace auction proceeds for %s:\n", auction.namespaceId);
    LogPrintf("  Total amount: %d RDD\n", winningBid.bidAmount / COIN);
    LogPrintf("  Burn pool: %d RDD (70%%)\n", (winningBid.bidAmount * 0.7) / COIN);
    LogPrintf("  Node operators: %d RDD (15%%)\n", (winningBid.bidAmount * 0.15) / COIN);
    LogPrintf("  Development fund: %d RDD (15%%)\n", (winningBid.bidAmount * 0.15) / COIN);
}
