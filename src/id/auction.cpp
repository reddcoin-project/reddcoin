// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
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
#include <map>
#include <set>
#include <string>
#include <vector>

AuctionManager::AuctionManager(ReddIDManager& manager) : reddIDManager(&manager) {
    // Initialize with ReddIDManager references
    reddidDB = manager.GetReddIDDB();
    reddidP2P = manager.GetP2PManager();
    namespaceManager = manager.GetNamespaceManager();
}

AuctionManager::~AuctionManager() {
    Save();
}

bool AuctionManager::Load() {
    LogPrintf("Loading auction data...\n");
    
    // Clear existing data
    auctions.clear();
    auctionBids.clear();
    activeAuctions.clear();
    
    // Load auctions
    std::vector<uint256> auctionIds;
    if (!reddidDB->ListAuctions(auctionIds)) {
        LogPrintf("Error: Failed to list auctions\n");
        return false;
    }
    
    for (const auto& auctionId : auctionIds) {
        AuctionInfo auction;
        if (reddidDB->ReadAuction(auctionId, auction)) {
            auctions[auctionId] = auction;
            
            // Add to active auctions if it's active
            if (auction.state == AUCTION_ACTIVE) {
                activeAuctions.insert(auctionId);
            }
            
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
                auctionBids[auctionId] = bids;
            }
        }
    }
    
    LogPrintf("Loaded %d auctions (%d active)\n", auctions.size(), activeAuctions.size());
    
    return true;
}

bool AuctionManager::Save() const {
    LogPrintf("Saving auction data...\n");
    
    // Save auctions
    for (const auto& pair : auctions) {
        if (!reddidDB->WriteAuction(pair.first, pair.second)) {
            LogPrintf("Error: Failed to save auction %s\n", pair.first.ToString());
            return false;
        }
        
        // Save bids
        auto it = auctionBids.find(pair.first);
        if (it != auctionBids.end()) {
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

bool AuctionManager::CreateAuction(AuctionInfo& auction, uint256& auctionId) {
    // Validate auction parameters
    if (!ValidateAuctionParameters(auction)) {
        return false;
    }
    
    // Generate auction ID
    auctionId = CalculateAuctionId(auction.name, auction.namespaceId, auction.startTime);
    auction.auctionId = auctionId;
    
    // Set auction state
    auction.state = AUCTION_PENDING;
    
    // Calculate deposit amount (10% of reserve price for regular IDs)
    auction.depositAmount = auction.reservePrice * 0.1;
    
    // Save to memory
    auctions[auctionId] = auction;
    auctionBids[auctionId] = std::vector<BidInfo>();
    
    // Save to database
    if (!reddidDB->WriteAuction(auctionId, auction)) {
        auctions.erase(auctionId);
        auctionBids.erase(auctionId);
        return false;
    }
    
    // Announce via P2P
    reddidP2P->AnnounceUserIDAuction(auction);
    
    return true;
}

bool AuctionManager::PlaceBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount, 
                            uint256& bidId, int64_t bidTime) {
    // Check if auction exists
    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
        return false;
    }
    
    AuctionInfo& auction = auctionIt->second;
    
    // Check if auction is active or pending
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        return false;
    }
    
    // If auction is pending, activate it
    if (auction.state == AUCTION_PENDING) {
        auction.state = AUCTION_ACTIVE;
        activeAuctions.insert(auctionId);
        reddidDB->WriteAuction(auctionId, auction);
    }
    
    // Validate bid
    if (!ValidateBid(auctionId, bidder, bidAmount)) {
        return false;
    }
    
    // Create bid
    BidInfo bid;
    bid.auctionId = auctionId;
    bid.bidder = bidder;
    bid.bidAmount = bidAmount;
    bid.depositAmount = auction.depositAmount;
    bid.bidTime = bidTime == 0 ? GetTime() : bidTime;
    bid.isWinner = false;
    bid.refunded = false;
    
    // Generate bid ID
    bidId = CalculateBidId(auctionId, bidder, bidAmount, bid.bidTime);
    bid.bidId = bidId;
    
    // Update auction
    auction.currentBid = bidAmount;
    auction.currentBidder = bidder;
    
    // Extend auction if needed (anti-sniping)
    ExtendAuctionIfNeeded(auction, bid.bidTime);
    
    // Save bid and updated auction
    auctionBids[auctionId].push_back(bid);
    
    if (!reddidDB->WriteBid(bidId, bid)) {
        return false;
    }
    
    if (!reddidDB->WriteAuction(auctionId, auction)) {
        return false;
    }
    
    // Announce bid via P2P
    reddidP2P->AnnounceUserIDBid(bid);
    
    return true;
}

bool AuctionManager::FinalizeAuction(const uint256& auctionId) {
    // Check if auction exists
    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
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
    activeAuctions.erase(auctionId);
    reddidDB->WriteAuction(auctionId, auction);
    
    // Check if there were any bids
    if (auction.currentBid == 0) {
        // No bids, cancel auction
        auction.state = AUCTION_CANCELED;
        reddidDB->WriteAuction(auctionId, auction);
        return true;
    }
    
    // Get highest bid
    auto bidsIt = auctionBids.find(auctionId);
    if (bidsIt == auctionBids.end() || bidsIt->second.empty()) {
        return false;
    }
    
    // Find winning bid
    BidInfo winningBid;
    bool foundWinner = false;
    CAmount highestBid = 0;
    for (const auto& bid : bidsIt->second) {
        if (bid.bidAmount > highestBid) {
            highestBid = bid.bidAmount;
            winningBid = bid;
            foundWinner = true;
        }
    }
    
    if (!foundWinner) {
        return false;
    }
    
    // Mark winning bid
    for (auto& bid : bidsIt->second) {
        if (bid.bidId == winningBid.bidId) {
            bid.isWinner = true;
            reddidDB->UpdateBidStatus(bid.bidId, true, false);
        }
    }
    
    // Create user ID
    UserIDInfo userID;
    userID.name = auction.name;
    userID.namespaceId = auction.namespaceId;
    userID.owner = winningBid.bidder;
    userID.registrationTime = currentTime;
    
    // Get namespace info for expiration time
    NamespaceInfo namespaceInfo;
    if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
        userID.expirationTime = currentTime + (namespaceInfo.renewalPeriod * 24 * 60 * 60);
    } else {
        // Default to 1 year if namespace info can't be loaded
        userID.expirationTime = currentTime + (365 * 24 * 60 * 60);
    }
    
    userID.lastTransaction = currentTime;
    userID.transactionCount = 1;
    
    // Save user ID
    if (!reddidDB->WriteUserID(auction.name, auction.namespaceId, userID)) {
        return false;
    }
    
    // Distribute auction proceeds
    DistributeAuctionProceeds(auction, winningBid);
    
    // Refund losing bids
    RefundLosingBids(auctionId, winningBid.bidId);
    
    // Mark auction as finalized
    auction.state = AUCTION_FINALIZED;
    reddidDB->WriteAuction(auctionId, auction);
    
    // Announce finalization via P2P
    reddidP2P->AnnounceUserIDFinalize(auctionId, winningBid.bidId, winningBid.bidAmount);
    
    return true;
}

bool AuctionManager::CancelAuction(const uint256& auctionId, const CKeyID& creator) {
    // Check if auction exists
    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
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
    activeAuctions.erase(auctionId);
    reddidDB->WriteAuction(auctionId, auction);
    
    // Announce cancellation via P2P
    reddidP2P->AnnounceUserIDCancel(auctionId);
    
    return true;
}

bool AuctionManager::ProcessExpiredAuctions(int64_t currentTime) {
    std::vector<uint256> expiredAuctions;
    
    // Find expired auctions
    for (const auto& auctionId : activeAuctions) {
        auto it = auctions.find(auctionId);
        if (it != auctions.end()) {
            const AuctionInfo& auction = it->second;
            if (auction.endTime <= currentTime) {
                expiredAuctions.push_back(auctionId);
            }
        }
    }
    
    // Finalize expired auctions
    for (const auto& auctionId : expiredAuctions) {
        LogPrintf("Finalizing expired auction: %s\n", auctionId.ToString());
        FinalizeAuction(auctionId);
    }
    
    return !expiredAuctions.empty();
}

bool AuctionManager::IsAuctionActive(const uint256& auctionId) const {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return false;
    }
    
    return it->second.state == AUCTION_ACTIVE;
}

bool AuctionManager::IsAuctionEnded(const uint256& auctionId) const {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return false;
    }
    
    return it->second.state == AUCTION_ENDED || 
           it->second.state == AUCTION_FINALIZED || 
           it->second.state == AUCTION_CANCELED;
}

bool AuctionManager::IsAuctionCreator(const uint256& auctionId, const CKeyID& keyId) const {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return false;
    }
    
    return it->second.creator == keyId;
}

bool AuctionManager::IsHighestBidder(const uint256& auctionId, const CKeyID& keyId) const {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return false;
    }
    
    return it->second.currentBidder == keyId;
}

bool AuctionManager::GetAuctionInfo(const uint256& auctionId, AuctionInfo& result) const {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return reddidDB->ReadAuction(auctionId, result);
    }
    
    result = it->second;
    return true;
}

bool AuctionManager::GetBidInfo(const uint256& bidId, BidInfo& result) const {
    // Search for bid in memory
    for (const auto& pair : auctionBids) {
        for (const auto& bid : pair.second) {
            if (bid.bidId == bidId) {
                result = bid;
                return true;
            }
        }
    }
    
    // Not found in memory, try database
    return reddidDB->ReadBid(bidId, result);
}

std::vector<AuctionInfo> AuctionManager::GetActiveAuctions() const {
    std::vector<AuctionInfo> result;
    
    for (const auto& auctionId : activeAuctions) {
        auto it = auctions.find(auctionId);
        if (it != auctions.end()) {
            result.push_back(it->second);
        }
    }
    
    return result;
}

std::vector<AuctionInfo> AuctionManager::GetAuctionsByNamespace(const std::string& namespaceId) const {
    std::vector<AuctionInfo> result;
    
    for (const auto& pair : auctions) {
        if (pair.second.namespaceId == namespaceId && pair.second.state == AUCTION_ACTIVE) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

std::vector<BidInfo> AuctionManager::GetAuctionBids(const uint256& auctionId) const {
    auto it = auctionBids.find(auctionId);
    if (it == auctionBids.end()) {
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

BidInfo AuctionManager::GetHighestBid(const uint256& auctionId) const {
    BidInfo highest;
    highest.bidAmount = 0;
    
    auto it = auctionBids.find(auctionId);
    if (it == auctionBids.end()) {
        // Try to load from database
        std::vector<uint256> bidIds;
        if (reddidDB->ListBids(auctionId, bidIds)) {
            for (const auto& bidId : bidIds) {
                BidInfo bid;
                if (reddidDB->ReadBid(bidId, bid) && bid.bidAmount > highest.bidAmount) {
                    highest = bid;
                }
            }
        }
    } else {
        // Find highest bid in memory
        for (const auto& bid : it->second) {
            if (bid.bidAmount > highest.bidAmount) {
                highest = bid;
            }
        }
    }
    
    return highest;
}

bool AuctionManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    // Process user ID auction transactions
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
                    if (opCode == OP_AUCTION_CREATE) {
                        // Parse user ID auction creation
                        LogPrintf("Received user ID auction creation transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_AUCTION_BID) {
                        // Parse user ID auction bid
                        LogPrintf("Received user ID auction bid transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_AUCTION_FINALIZE) {
                        // Parse user ID auction finalization
                        LogPrintf("Received user ID auction finalization transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                    else if (opCode == OP_AUCTION_CANCEL) {
                        // Parse user ID auction cancellation
                        LogPrintf("Received user ID auction cancellation transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

void AuctionManager::CheckExpiredAuctions() {
    int64_t currentTime = GetTime();
    
    if (ProcessExpiredAuctions(currentTime)) {
        LogPrintf("Processed expired auctions at time %d\n", currentTime);
    }
}

bool AuctionManager::ValidateAuctionParameters(const AuctionInfo& auction) {
    // Check if name and namespace are valid
    if (auction.name.empty() || auction.namespaceId.empty()) {
        return false;
    }
    
    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
        return false;
    }
    
    // Check if user ID is valid based on namespace rules
    if (auction.name.size() < namespaceInfo.minLength || auction.name.size() > namespaceInfo.maxLength) {
        return false;
    }
    
    // Check if reserve price is valid
    if (auction.reservePrice <= 0) {
        return false;
    }
    
    // Check if auction duration is valid
    int durationDays = (auction.endTime - auction.startTime) / (24 * 60 * 60);
    if (durationDays < namespaceInfo.minAuctionDuration || 
        durationDays > namespaceInfo.maxAuctionDuration) {
        return false;
    }
    
    return true;
}

bool AuctionManager::ValidateBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) {
    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
        return false;
    }
    
    const AuctionInfo& auction = auctionIt->second;
    
    // Check if bid meets reserve price
    if (bidAmount < auction.reservePrice) {
        return false;
    }
    
    // Check if bid is higher than current bid
    if (auction.currentBid > 0) {
        // Get namespace for bid increment
        NamespaceInfo namespaceInfo;
        double minIncrement = 0.05; // Default 5%
        
        if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
            minIncrement = namespaceInfo.minBidIncrement / 100.0;
        }
        
        CAmount minBid = auction.currentBid * (1.0 + minIncrement);
        if (bidAmount < minBid) {
            return false;
        }
    }
    
    return true;
}

uint256 AuctionManager::CalculateAuctionId(const std::string& name, const std::string& namespaceId, int64_t startTime) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << name;
    ss << namespaceId;
    ss << startTime;
    
    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

uint256 AuctionManager::CalculateBidId(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount, int64_t bidTime) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << auctionId;
    ss << bidder;
    ss << bidAmount;
    ss << bidTime;
    
    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

void AuctionManager::DistributeAuctionProceeds(const AuctionInfo& auction, const BidInfo& winningBid) {
    // Get namespace info for distribution percentages
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
        // Use default percentages if namespace info not available
        LogPrintf("Distributing auction proceeds for %s.%s:\n", auction.name, auction.namespaceId);
        LogPrintf("  Total amount: %d RDD\n", winningBid.bidAmount / COIN);
        LogPrintf("  Burn pool: %d RDD (70%%)\n", (winningBid.bidAmount * 0.7) / COIN);
        LogPrintf("  Namespace owner: %d RDD (10%%)\n", (winningBid.bidAmount * 0.1) / COIN);
        LogPrintf("  Node operators: %d RDD (15%%)\n", (winningBid.bidAmount * 0.15) / COIN);
        LogPrintf("  Development fund: %d RDD (5%%)\n", (winningBid.bidAmount * 0.05) / COIN);
        return;
    }
    
    // Calculate distribution based on namespace settings
    double burnPct = namespaceInfo.burnPct / 100.0;
    double namespacePct = namespaceInfo.namespaceRevenuePct / 100.0;
    double nodePct = namespaceInfo.nodePct / 100.0;
    double devPct = namespaceInfo.devPct / 100.0;
    
    LogPrintf("Distributing auction proceeds for %s.%s:\n", auction.name, auction.namespaceId);
    LogPrintf("  Total amount: %d RDD\n", winningBid.bidAmount / COIN);
    LogPrintf("  Burn pool: %d RDD (%d%%)\n", (winningBid.bidAmount * burnPct) / COIN, namespaceInfo.burnPct);
    LogPrintf("  Namespace owner: %d RDD (%d%%)\n", (winningBid.bidAmount * namespacePct) / COIN, namespaceInfo.namespaceRevenuePct);
    LogPrintf("  Node operators: %d RDD (%d%%)\n", (winningBid.bidAmount * nodePct) / COIN, namespaceInfo.nodePct);
    LogPrintf("  Development fund: %d RDD (%d%%)\n", (winningBid.bidAmount * devPct) / COIN, namespaceInfo.devPct);
}

bool AuctionManager::RefundLosingBids(const uint256& auctionId, const uint256& winningBidId) {
    auto it = auctionBids.find(auctionId);
    if (it == auctionBids.end()) {
        return false;
    }
    
    for (auto& bid : it->second) {
        if (bid.bidId != winningBidId && !bid.refunded) {
            // Refund would happen in a real implementation
            // For now, just mark as refunded
            bid.refunded = true;
            reddidDB->UpdateBidStatus(bid.bidId, false, true);
        }
    }
    
    return true;
}

bool AuctionManager::UpdateAuctionState(uint256& auctionId, AuctionState newState) {
    auto it = auctions.find(auctionId);
    if (it == auctions.end()) {
        return false;
    }
    
    // Update state
    it->second.state = newState;
    
    // Update active auctions set
    if (newState == AUCTION_ACTIVE) {
        activeAuctions.insert(auctionId);
    } else {
        activeAuctions.erase(auctionId);
    }
    
    // Save to database
    return reddidDB->WriteAuction(auctionId, it->second);
}

bool AuctionManager::ExtendAuctionIfNeeded(AuctionInfo& auction, int64_t bidTime) {
    // Check if this is a late bid (less than 24 hours before end)
    int64_t timeLeft = auction.endTime - bidTime;
    if (timeLeft < 24 * 60 * 60) { // Less than 24 hours left
        // Extend by 12 hours
        auction.endTime = bidTime + 12 * 60 * 60;
        LogPrintf("Extending auction for %s.%s by 12 hours due to late bid\n", 
                 auction.name, auction.namespaceId);
        return true;
    }
    
    return false;
}
