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

AuctionManager::AuctionManager(ReddIDManager& manager) :
    reddIDManager(&manager),
    reddidDB(nullptr),
    reddidP2P(nullptr),
    namespaceManager(nullptr),
    chainstate(nullptr) {
    // Initialize with ReddIDManager references
    reddidDB = manager.GetReddIDDB();
    reddidP2P = manager.GetP2PManager();
    namespaceManager = manager.GetNamespaceManager();
    chainstate = manager.GetChainState();

    LogPrint(BCLog::REDDID, "AuctionManager initialized\n");
}

AuctionManager::~AuctionManager() {
    Save();
}

bool AuctionManager::Load() {
    LogPrint(BCLog::REDDID, "Loading auction data...\n");

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

    LogPrint(BCLog::REDDID, "Loaded %d auctions (%d active)\n", auctions.size(), activeAuctions.size());

    return true;
}

bool AuctionManager::Save() const {
    LogPrint(BCLog::REDDID, "Saving auction data...\n");

    if (!reddidDB) {
        LogPrintf("Error: AuctionManager::Save: Database not initialized\n");
        return false;
    }

    try {
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
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::Save: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::CreateAuction(AuctionInfo& auction, uint256& auctionId) {
    if (!reddIDManager || !namespaceManager || !reddidDB) {
        LogPrintf("Error: AuctionManager::CreateAuction: Not initialized\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Creating auction for %s.%s\n", auction.name, auction.namespaceId);

    try {
        // Validate auction parameters
        if (!ValidateAuctionParameters(auction)) {
            LogPrintf("Error: AuctionManager::CreateAuction: Invalid auction parameters\n");
            return false;
        }

        // Generate auction ID
        auctionId = CalculateAuctionId(auction.name, auction.namespaceId, auction.startTime);
        auction.auctionId = auctionId;

        // Check if auction already exists
        if (auctions.find(auctionId) != auctions.end()) {
            LogPrintf("Error: AuctionManager::CreateAuction: Auction already exists: %s\n", auctionId.ToString());
            return false;
        }

        // Set auction state
        auction.state = AUCTION_PENDING;

        // Calculate deposit amount (10% of reserve price for regular IDs)
        auction.depositAmount = auction.reservePrice * 0.1;

        // Set block height
        auction.blockHeight = chainstate ? chainstate->m_chain.Height() : 0;

        // Save to memory
        auctions[auctionId] = auction;
        auctionBids[auctionId] = std::vector<BidInfo>();

        // Save to database
        if (!reddidDB->WriteAuction(auctionId, auction)) {
            auctions.erase(auctionId);
            auctionBids.erase(auctionId);
            LogPrintf("Error: AuctionManager::CreateAuction: Failed to write auction to database\n");
            return false;
        }

        // Announce via P2P
        if (reddidP2P) {
            if (auction.name.empty()) {
                // Namespace auction
                reddidP2P->AnnounceNamespaceAuction(auction);
            } else {
                // User ID auction
                reddidP2P->AnnounceUserIDAuction(auction);
            }
        }

        LogPrint(BCLog::REDDID, "Successfully created auction %s for %s.%s\n", 
                auctionId.ToString(), auction.name, auction.namespaceId);
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::CreateAuction: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::PlaceBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount,
                            uint256& bidId, int64_t bidTime) {
    if (!reddIDManager || !reddidDB) {
        LogPrintf("Error: AuctionManager::PlaceBid: Not initialized\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Placing bid on auction %s: %s RDD\n", auctionId.ToString(), FormatMoney(bidAmount));

    try {
        // Check if auction exists
        auto auctionIt = auctions.find(auctionId);
        if (auctionIt == auctions.end()) {
            LogPrintf("Error: AuctionManager::PlaceBid: Auction not found: %s\n", auctionId.ToString());
            return false;
        }

        AuctionInfo& auction = auctionIt->second;

        // Check if auction is active or pending
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
            LogPrintf("Error: AuctionManager::PlaceBid: Auction not active or pending: %s, state: %d\n",
                     auctionId.ToString(), auction.state);
            return false;
        }

        // If auction is pending, activate it
        if (auction.state == AUCTION_PENDING) {
            auction.state = AUCTION_ACTIVE;
            activeAuctions.insert(auctionId);

            if (!reddidDB->WriteAuction(auctionId, auction)) {
                LogPrintf("Error: AuctionManager::PlaceBid: Failed to update auction state to active\n");
                activeAuctions.erase(auctionId);  // Revert in-memory change on database failure
                return false;
            }
        }

        // Check if bid is from the auction creator (self-bidding prevention)
        if (auction.creator == bidder) {
            LogPrintf("Error: AuctionManager::PlaceBid: Creator cannot bid on own auction: %s\n",
                     auctionId.ToString());
            return false;
        }

        // Validate bid
        if (!ValidateBid(auctionId, bidder, bidAmount)) {
            LogPrintf("Error: AuctionManager::PlaceBid: Invalid bid\n");
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

        // Check for existing bid from same bidder
        bool existingBidFound = false;
        for (auto& existingBid : auctionBids[auctionId]) {
            if (existingBid.bidder == bidder) {
                LogPrint(BCLog::REDDID, "Found existing bid from same bidder, updating: %s\n",
                        existingBid.bidId.ToString());
                existingBidFound = true;
                // Handle existing bid - either replace or reject
                if (existingBid.bidAmount >= bidAmount) {
                    LogPrintf("Error: AuctionManager::PlaceBid: New bid amount less than existing bid\n");
                    return false;
                }
                // Mark old bid for refund or update
                existingBid.refunded = true;
                if (!reddidDB->UpdateBidStatus(existingBid.bidId, false, true)) {
                    LogPrintf("Error: AuctionManager::PlaceBid: Failed to update existing bid status\n");
                }
                break;
            }
        }

        // Update auction
        auction.currentBid = bidAmount;
        auction.currentBidder = bidder;

        // Extend auction if needed (anti-sniping)
        ExtendAuctionIfNeeded(auction, bid.bidTime);

        // Save bid and updated auction
        auctionBids[auctionId].push_back(bid);

        if (!reddidDB->WriteBid(bidId, bid)) {
            // Revert in-memory changes on database failure
            auctionBids[auctionId].pop_back();
            LogPrintf("Error: AuctionManager::PlaceBid: Failed to write bid to database\n");
            return false;
        }

        if (!reddidDB->WriteAuction(auctionId, auction)) {
            // Try to remove the bid if auction update fails
            try {
                reddidDB->EraseBid(bidId);
                auctionBids[auctionId].pop_back();
            } catch (const std::exception& e) {
                LogPrintf("Error: AuctionManager::PlaceBid: Failed to clean up bid after auction update failure: %s\n",
                         e.what());
            }
            LogPrintf("Error: AuctionManager::PlaceBid: Failed to update auction in database\n");
            return false;
        }

        // Announce bid via P2P
        if (reddidP2P) {
            if (auction.name.empty()) {
                // Namespace auction
                reddidP2P->AnnounceNamespaceBid(bid);
            } else {
                // User ID auction
                reddidP2P->AnnounceUserIDBid(bid);
            }
        }

        LogPrint(BCLog::REDDID, "Successfully placed bid %s on auction %s: %s RDD\n",
                bidId.ToString(), auctionId.ToString(), FormatMoney(bidAmount));
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::PlaceBid: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::FinalizeAuction(const uint256& auctionId) {
    if (!reddIDManager || !reddidDB || chainstate == nullptr) {
        LogPrintf("Error: AuctionManager::FinalizeAuction: Not initialized\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Finalizing auction %s\n", auctionId.ToString());

    try {
        // Check if auction exists
        auto auctionIt = auctions.find(auctionId);
        if (auctionIt == auctions.end()) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: Auction not found: %s\n", auctionId.ToString());
            return false;
        }

        AuctionInfo& auction = auctionIt->second;

        // Check if auction is active or ended
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: Auction not active or ended: %s, state: %d\n",
                     auctionId.ToString(), auction.state);
            return false;
        }

        // Check if auction has ended naturally
        int64_t currentTime = GetTime();
        if (auction.endTime > currentTime) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: Auction has not ended yet: %s\n", auctionId.ToString());
            return false;
        }

        // Mark auction as ended
        auction.state = AUCTION_ENDED;
        activeAuctions.erase(auctionId);

        if (!reddidDB->WriteAuction(auctionId, auction)) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to update auction state\n");
            return false;
        }

        // Check if there were any bids
        if (auction.currentBid == 0) {
            // No bids, cancel auction
            auction.state = AUCTION_CANCELED;

            if (!reddidDB->WriteAuction(auctionId, auction)) {
                LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to cancel auction\n");
                return false;
            }

            LogPrint(BCLog::REDDID, "Auction %s canceled due to no bids\n", auctionId.ToString());
            return true;
        }

        // Get highest bid
        auto bidsIt = auctionBids.find(auctionId);
        if (bidsIt == auctionBids.end() || bidsIt->second.empty()) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: No bids found for auction: %s\n", auctionId.ToString());
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

        if (highestBid == 0) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: No valid bids found\n");
            return false;
        }

        // Mark winning bid
        for (auto& bid : bidsIt->second) {
            if (bid.bidId == winningBid.bidId) {
                bid.isWinner = true;

                if (!reddidDB->UpdateBidStatus(bid.bidId, true, false)) {
                    LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to update bid status\n");
                    return false;
                }
            }
        }

        // Process auction result
        if (auction.name.empty()) {
            // This is a namespace auction
            // Let the NamespaceManager handle namespace creation
            bool success = false;
            if (namespaceManager) {
                success = namespaceManager->FinalizeNamespaceAuction(auctionId);
            }

            if (!success) {
                LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to create namespace\n");
                return false;
            }
        } else {
            // This is a user ID auction
            UserIDInfo userID;
            userID.name = auction.name;
            userID.namespaceId = auction.namespaceId;
            userID.owner = winningBid.bidder;
            userID.registrationTime = currentTime;

            // Get namespace info for expiration time
            NamespaceInfo namespaceInfo;
            if (namespaceManager && namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
                userID.expirationTime = currentTime + (namespaceInfo.renewalPeriod * 24 * 60 * 60);
            } else {
                // Default to 1 year if namespace info can't be loaded
                userID.expirationTime = currentTime + (365 * 24 * 60 * 60);
            }

            userID.lastTransaction = currentTime;
            userID.transactionCount = 1;

            // Save user ID
            if (!reddidDB->WriteUserID(auction.name, auction.namespaceId, userID)) {
                LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to create user ID\n");
                return false;
            }
        }

        // Distribute auction proceeds
        DistributeAuctionProceeds(auction, winningBid);

        // Refund losing bids
        RefundLosingBids(auctionId, winningBid.bidId);

        // Mark auction as finalized
        auction.state = AUCTION_FINALIZED;

        if (!reddidDB->WriteAuction(auctionId, auction)) {
            LogPrintf("Error: AuctionManager::FinalizeAuction: Failed to update final auction state\n");
            return false;
        }

        // Announce finalization via P2P
        if (reddidP2P) {
            if (auction.name.empty()) {
                // Namespace auction
                reddidP2P->AnnounceNamespaceFinalize(auctionId, winningBid.bidId, winningBid.bidAmount);
            } else {
                // User ID auction
                reddidP2P->AnnounceUserIDFinalize(auctionId, winningBid.bidId, winningBid.bidAmount);
            }
        }

        LogPrint(BCLog::REDDID, "Successfully finalized auction %s, winner: %s, amount: %s RDD\n",
                auctionId.ToString(), winningBid.bidder.ToString(), FormatMoney(winningBid.bidAmount));
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::FinalizeAuction: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::CancelAuction(const uint256& auctionId, const CKeyID& creator) {
    if (!reddIDManager || !reddidDB || chainstate == nullptr) {
        LogPrintf("Error: AuctionManager::CancelAuction: Not initialized\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Canceling auction %s\n", auctionId.ToString());

    try {
        // Check if auction exists
        auto auctionIt = auctions.find(auctionId);
        if (auctionIt == auctions.end()) {
            LogPrintf("Error: AuctionManager::CancelAuction: Auction not found: %s\n", auctionId.ToString());
            return false;
        }

        AuctionInfo& auction = auctionIt->second;

        // Check if creator matches
        if (auction.creator != creator) {
            LogPrintf("Error: AuctionManager::CancelAuction: Not authorized to cancel auction: %s\n", auctionId.ToString());
            return false;
        }

        // Check if auction can be canceled (only pending or no bids)
        if (auction.state != AUCTION_PENDING && auction.currentBid > 0) {
            LogPrintf("Error: AuctionManager::CancelAuction: Cannot cancel auction with bids: %s\n", auctionId.ToString());
            return false;
        }

        // Mark auction as canceled
        auction.state = AUCTION_CANCELED;
        activeAuctions.erase(auctionId);

        if (!reddidDB->WriteAuction(auctionId, auction)) {
            LogPrintf("Error: AuctionManager::CancelAuction: Failed to update auction state\n");
            return false;
        }

        // Announce cancellation via P2P
        if (reddidP2P) {
            if (auction.name.empty()) {
                // Namespace auction
                reddidP2P->AnnounceNamespaceCancel(auctionId);
            } else {
                // User ID auction
                reddidP2P->AnnounceUserIDCancel(auctionId);
            }
        }

        LogPrint(BCLog::REDDID, "Successfully canceled auction %s\n", auctionId.ToString());
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::CancelAuction: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::ProcessExpiredAuctions(int64_t currentTime) {
    LogPrint(BCLog::REDDID, "Processing expired auctions at time %d\n", currentTime);

    if (!reddIDManager || !reddidDB) {
        LogPrintf("Error: AuctionManager::ProcessExpiredAuctions: Not initialized\n");
        return false;
    }

    try {
        std::vector<uint256> expiredActiveAuctions;
        std::vector<uint256> expiredPendingAuctions;

        // Find expired active auctions
        for (const auto& auctionId : activeAuctions) {
            auto it = auctions.find(auctionId);
            if (it != auctions.end()) {
                const AuctionInfo& auction = it->second;
                if (auction.endTime <= currentTime) {
                    expiredActiveAuctions.push_back(auctionId);
                }
            }
        }

        // Find expired pending auctions
        for (const auto& pair : auctions) {
            const AuctionInfo& auction = pair.second;
            if (auction.state == AUCTION_PENDING && auction.endTime <= currentTime) {
                expiredPendingAuctions.push_back(pair.first);
            }
        }

        LogPrint(BCLog::REDDID, "Found %d expired active auctions and %d expired pending auctions\n",
                 expiredActiveAuctions.size(), expiredPendingAuctions.size());

        // Finalize expired active auctions
        for (const auto& auctionId : expiredActiveAuctions) {
            LogPrint(BCLog::REDDID, "Finalizing expired active auction: %s\n", auctionId.ToString());
            FinalizeAuction(auctionId);
        }

        // Cancel expired pending auctions
        for (auto auctionId : expiredPendingAuctions) {  // Note: removed const& since UpdateAuctionState takes non-const
            LogPrint(BCLog::REDDID, "Canceling expired pending auction: %s\n", auctionId.ToString());

            // Use UpdateAuctionState to properly handle the state change
            if (!UpdateAuctionState(auctionId, AUCTION_CANCELED)) {
                LogPrintf("Error: Failed to cancel expired pending auction: %s\n", auctionId.ToString());
                continue;
            }

            // Announce cancellation via P2P if appropriate
            if (reddidP2P) {
                auto it = auctions.find(auctionId);
                if (it != auctions.end()) {
                    const AuctionInfo& auction = it->second;
                    if (auction.name.empty()) {
                        // Namespace auction
                        reddidP2P->AnnounceNamespaceCancel(auctionId);
                    } else {
                        // User ID auction
                        reddidP2P->AnnounceUserIDCancel(auctionId);
                    }
                }
            }
        }

        return !expiredActiveAuctions.empty() || !expiredPendingAuctions.empty();
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::ProcessExpiredAuctions: Exception: %s\n", e.what());
        return false;
    }
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
        if (!reddidDB) {
            LogPrintf("Error: AuctionManager::GetAuctionInfo: Database not initialized\n");
            return false;
        }
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
    if (!reddidDB) {
        LogPrintf("Error: AuctionManager::GetBidInfo: Database not initialized\n");
        return false;
    }
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

        if (!reddidDB || !reddidDB->ListBids(auctionId, bidIds)) {
            return result;
        }

        for (const auto& bidId : bidIds) {
            BidInfo bid;
            if (reddidDB->ReadBid(bidId, bid)) {
                result.push_back(bid);
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
        if (reddidDB && reddidDB->ListBids(auctionId, bidIds)) {
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
    if (!reddIDManager || !reddidDB || chainstate == nullptr) {
        LogPrintf("Error: AuctionManager::ProcessTransaction: Not initialized\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Processing transaction %s for auction operations\n", tx.GetHash().ToString());

    try {
        // First get the sender for validation
        CKeyID sender;
        if (!GetTransactionSender(tx, sender)) {
            LogPrintf("Error: AuctionManager::ProcessTransaction: Failed to determine transaction sender\n");
            return false;
        }

        // Process auction transactions
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
                            LogPrint(BCLog::REDDID, "Received user ID auction creation transaction: %s\n", tx.GetHash().ToString());

                            // Extract user ID details
                            std::string content(vchData.begin() + 2, vchData.end());
                            size_t pos = content.find('.');
                            if (pos != std::string::npos) {
                                std::string name = content.substr(0, pos);
                                std::string namespaceId = content.substr(pos + 1);

                                // Create auction
                                AuctionInfo auction;
                                auction.name = name;
                                auction.namespaceId = namespaceId;
                                auction.startTime = GetTime();
                                auction.endTime = auction.startTime + (7 * 24 * 60 * 60);  // Default 7 days
                                auction.reservePrice = 1000 * COIN;  // Default 1000 RDD
                                auction.state = AUCTION_PENDING;
                                auction.txid = tx.GetHash();
                                auction.creator = sender;

                                // Create auction
                                uint256 auctionId;
                                if (CreateAuction(auction, auctionId)) {
                                    return true;
                                }
                            }
                        } else if (opCode == OP_AUCTION_BID) {
                            // Parse user ID auction bid
                            LogPrint(BCLog::REDDID, "Received user ID auction bid transaction: %s\n", tx.GetHash().ToString());

                            // Extract auction ID and bid amount
                            if (vchData.size() >= 34) {  // 'R' + opcode + auctionId (32 bytes)
                                uint256 auctionId;
                                std::vector<unsigned char> auctionIdData(vchData.begin() + 2, vchData.begin() + 34);
                                memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

                                CAmount bidAmount = 0;
                                if (vchData.size() >= 42) {  // 'R' + opcode + auctionId (32) + bidAmount (8)
                                    std::vector<unsigned char> bidAmountData(vchData.begin() + 34, vchData.begin() + 42);
                                    bidAmount = ReadLE64(bidAmountData.data());
                                } else {
                                    // Default to transaction amount
                                    for (const auto& txout : tx.vout) {
                                        if (!txout.scriptPubKey.IsUnspendable()) {
                                            bidAmount = std::max(bidAmount, txout.nValue);
                                        }
                                    }
                                }

                                // Place bid
                                uint256 bidId;
                                if (PlaceBid(auctionId, sender, bidAmount, bidId)) {
                                    return true;
                                }
                            }
                        } else if (opCode == OP_AUCTION_FINALIZE) {
                            // Parse user ID auction finalization
                            LogPrint(BCLog::REDDID, "Received user ID auction finalization transaction: %s\n", tx.GetHash().ToString());

                            // Extract auction ID
                            if (vchData.size() >= 34) {  // 'R' + opcode + auctionId (32 bytes)
                                uint256 auctionId;
                                std::vector<unsigned char> auctionIdData(vchData.begin() + 2, vchData.begin() + 34);
                                memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

                                // Verify sender is the auction creator or has finalization rights
                                AuctionInfo auction;
                                if (!GetAuctionInfo(auctionId, auction)) {
                                    LogPrintf("Error: AuctionManager::ProcessTransaction: Auction not found: %s\n", auctionId.ToString());
                                    return false;
                                }

                                if (auction.creator != sender) {
                                    // Only allow auction creator to finalize
                                    LogPrintf("Error: AuctionManager::ProcessTransaction: Unauthorized finalization attempt: %s\n", sender.ToString());
                                    return false;
                                }

                                // Finalize auction
                                if (FinalizeAuction(auctionId)) {
                                    return true;
                                }
                            }
                        } else if (opCode == OP_AUCTION_CANCEL) {
                            // Parse user ID auction cancellation
                            LogPrint(BCLog::REDDID, "Received user ID auction cancellation transaction: %s\n", tx.GetHash().ToString());

                            // Extract auction ID
                            if (vchData.size() >= 34) {  // 'R' + opcode + auctionId (32 bytes)
                                uint256 auctionId;
                                std::vector<unsigned char> auctionIdData(vchData.begin() + 2, vchData.begin() + 34);
                                memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

                                // Cancel auction
                                if (CancelAuction(auctionId, sender)) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }

        return false;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::ProcessTransaction: Exception: %s\n", e.what());
        return false;
    }
}

// Helper to get the transaction sender
bool AuctionManager::GetTransactionSender(const CTransaction& tx, CKeyID& sender) {
    if (chainstate == nullptr) {
        LogPrintf("Error: AuctionManager::GetTransactionSender: Chain state not initialized\n");
        return false;
    }

    // Find the first input with a valid address
    for (const auto& txin : tx.vin) {
        Coin coin;
        if (!chainstate->CoinsTip().GetCoin(txin.prevout, coin)) {
            continue;
        }

        CTxDestination address;
        if (ExtractDestination(coin.out.scriptPubKey, address)) {
            const PKHash* pkHash = std::get_if<PKHash>(&address);
            if (pkHash) {
                sender = CKeyID(uint160(*pkHash));
                return true;
            }
        }
    }
    return false;
}

bool AuctionManager::VerifyTransactionOwnership(const CTransaction& tx, const CKeyID& expectedOwner) {
    if (chainstate == nullptr) {
        LogPrintf("Error: AuctionManager::VerifyTransactionOwnership: Chain state not initialized\n");
        return false;
    }

    CKeyID actualSender;
    if (!GetTransactionSender(tx, actualSender)) {
        return false;
    }

    return (actualSender == expectedOwner);
}

void AuctionManager::CheckExpiredAuctions() {
    ProcessExpiredAuctions(GetTime());
}

bool AuctionManager::ValidateAuctionParameters(const AuctionInfo& auction) {
    LogPrint(BCLog::REDDID, "Validating auction parameters for %s.%s\n", auction.name, auction.namespaceId);

    try {
        // Check for empty namespace ID
        if (auction.namespaceId.empty()) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Empty namespace ID\n");
            return false;
        }

        // Check if namespace exists
        NamespaceInfo namespaceInfo;
        if (!namespaceManager) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Namespace manager not available\n");
            return false;
        }

        if (!namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Namespace not found: %s\n", auction.namespaceId);
            return false;
        }

        // Check for invalid creator key
        if (auction.creator.IsNull()) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid creator key\n");
            return false;
        }

        // Check auction timing
        if (auction.startTime <= 0 || auction.endTime <= auction.startTime) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid auction timing: start=%d, end=%d\n",
                     auction.startTime, auction.endTime);
            return false;
        }

        // Check auction duration
        int64_t durationSeconds = auction.endTime - auction.startTime;
        int durationDays = durationSeconds / (24 * 60 * 60);

        if (durationDays < namespaceInfo.minAuctionDuration || durationDays > namespaceInfo.maxAuctionDuration) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid auction duration: %d days (allowed: %d-%d)\n",
                     durationDays, namespaceInfo.minAuctionDuration, namespaceInfo.maxAuctionDuration);
            return false;
        }

        // Check for negative reserve price
        if (auction.reservePrice < 0) {
            LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Negative reserve price: %s\n",
                     FormatMoney(auction.reservePrice));
            return false;
        }

        // If this is a user ID auction, validate name
        if (!auction.name.empty()) {
            // Check length
            if (auction.name.size() < namespaceInfo.minLength || auction.name.size() > namespaceInfo.maxLength) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid name length: %zu (allowed: %d-%d)\n",
                         auction.name.size(), namespaceInfo.minLength, namespaceInfo.maxLength);
                return false;
            }

            // Check allowed characters
            for (const char& c : auction.name) {
                bool isValidChar = false;

                // Letters are always allowed
                if (c >= 'a' && c <= 'z') {
                    isValidChar = true;
                } else if (namespaceInfo.allowNumbers && c >= '0' && c <= '9') {  // Check if numbers are allowed
                    isValidChar = true;
                } else if (namespaceInfo.allowHyphens && c == '-') {  // Check if hyphens are allowed
                    isValidChar = true;
                } else if (namespaceInfo.allowUnderscores && c == '_') {  // Check if underscores are allowed
                    isValidChar = true;
                }

                if (!isValidChar) {
                    LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid character in name: %c\n", c);
                    return false;
                }
            }

            // Cannot begin or end with hyphen
            if (auction.name[0] == '-' || auction.name[auction.name.size() - 1] == '-') {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Name cannot begin or end with a hyphen\n");
                return false;
            }

            // Check consecutive hyphens if not allowed
            if (namespaceInfo.allowHyphens && auction.name.find("--") != std::string::npos) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Consecutive hyphens not allowed\n");
                return false;
            }

            // Check if already registered
            if (!reddidDB) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Database not available\n");
                return false;
            }

            UserIDInfo existingUserID;
            if (reddidDB->ReadUserID(auction.name, auction.namespaceId, existingUserID)) {
                // Check if expired
                int64_t currentTime = GetTime();
                if (existingUserID.expirationTime > currentTime) {
                    LogPrintf("Error: AuctionManager::ValidateAuctionParameters: User ID already registered: %s.%s\n",
                             auction.name, auction.namespaceId);
                    return false;
                }

                // Allow auctions for expired IDs
                LogPrint(BCLog::REDDID, "User ID is expired, allowing renewal auction: %s.%s\n",
                        auction.name, auction.namespaceId);
            }

            // For user IDs, check if reserve price meets minimum
            CAmount minPrice;
            if (reddIDManager) {
                minPrice = reddIDManager->CalculateMinPriceForUserID(auction.name, auction.namespaceId);
            } else {
                // Fallback to default minimum if ReddIDManager not available
                minPrice = REDDID_MIN_FEE;
            }

            if (auction.reservePrice < minPrice) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Reserve price too low: %s (minimum: %s)\n",
                         FormatMoney(auction.reservePrice), FormatMoney(minPrice));
                return false;
            }
        } else {
            // This is a namespace auction, validate namespace ID
            if (!namespaceManager->ValidateNamespaceID(auction.namespaceId)) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Invalid namespace ID: %s\n", auction.namespaceId);
                return false;
            }

            if (!namespaceManager->IsNamespaceAvailable(auction.namespaceId)) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Namespace not available: %s\n", auction.namespaceId);
                return false;
            }

            // Check if reserve price meets minimum for namespaces
            CAmount minPrice;
            if (reddIDManager) {
                minPrice = reddIDManager->CalculateMinPriceForNamespace(auction.namespaceId);
            } else {
                // Fallback to default minimum if ReddIDManager not available
                minPrice = NAMESPACE_MIN_FEE;
            }

            if (auction.reservePrice < minPrice) {
                LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Reserve price too low: %s (minimum: %s)\n",
                         FormatMoney(auction.reservePrice), FormatMoney(minPrice));
                return false;
            }
        }

        LogPrint(BCLog::REDDID, "Auction parameters valid for %s.%s\n", auction.name, auction.namespaceId);
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::ValidateAuctionParameters: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::ValidateBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) {
    LogPrint(BCLog::REDDID, "Validating bid on auction %s: %s RDD\n", auctionId.ToString(), FormatMoney(bidAmount));

    try {
        // Get auction info
        auto auctionIt = auctions.find(auctionId);
        if (auctionIt == auctions.end()) {
            LogPrintf("Error: AuctionManager::ValidateBid: Auction not found: %s\n", auctionId.ToString());
            return false;
        }

        const AuctionInfo& auction = auctionIt->second;

        // Check if auction is active or pending
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
            LogPrintf("Error: AuctionManager::ValidateBid: Auction not active or pending: %s\n", auctionId.ToString());
            return false;
        }

        // Check if auction has ended
        int64_t currentTime = GetTime();
        if (auction.endTime <= currentTime) {
            LogPrintf("Error: AuctionManager::ValidateBid: Auction has ended: %s\n", auctionId.ToString());
            return false;
        }

        // Check if bid meets minimum price
        if (bidAmount < auction.reservePrice) {
            LogPrintf("Error: AuctionManager::ValidateBid: Bid amount too low: %s (minimum: %s)\n",
                     FormatMoney(bidAmount), FormatMoney(auction.reservePrice));
            return false;
        }

        // If there are existing bids, check if this bid meets minimum increment
        if (auction.currentBid > 0) {
            // Get namespace for minimum bid increment
            double minIncrementPct = 0.05;  // Default 5%

            if (namespaceManager) {
                NamespaceInfo namespaceInfo;
                if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
                    minIncrementPct = namespaceInfo.minBidIncrement / 100.0;
                }
            }

            CAmount minBid = auction.currentBid * (1.0 + minIncrementPct);
            if (bidAmount < minBid) {
                LogPrintf("Error: AuctionManager::ValidateBid: Bid increment too low: %s (minimum: %s)\n",
                         FormatMoney(bidAmount), FormatMoney(minBid));
                return false;
            }
        }

        LogPrint(BCLog::REDDID, "Bid valid for auction %s: %s RDD\n", auctionId.ToString(), FormatMoney(bidAmount));
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::ValidateBid: Exception: %s\n", e.what());
        return false;
    }
}

uint256 AuctionManager::CalculateAuctionId(const std::string& name, const std::string& namespaceId, int64_t startTime) {
    // Create a deterministic auction ID based on name, namespace, and start time
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << name;
    ss << namespaceId;
    ss << startTime;

    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

uint256 AuctionManager::CalculateBidId(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount, int64_t bidTime) {
    // Create a deterministic bid ID based on auction, bidder, amount, and time
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << auctionId;
    ss << bidder;
    ss << bidAmount;
    ss << bidTime;

    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

void AuctionManager::DistributeAuctionProceeds(const AuctionInfo& auction, const BidInfo& winningBid) {
    LogPrint(BCLog::REDDID, "Distributing auction proceeds for %s.%s\n", auction.name, auction.namespaceId);

    try {
        NamespaceInfo namespaceInfo;
        int burnPct = 70;        // Default: 70%
        int namespacePct = 10;   // Default: 10%
        int nodePct = 15;        // Default: 15%
        int devPct = 5;          // Default: 5%

        // Get namespace info for distribution percentages
        if (namespaceManager && namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
            burnPct = namespaceInfo.burnPct;
            namespacePct = namespaceInfo.namespaceRevenuePct;
            nodePct = namespaceInfo.nodePct;
            devPct = namespaceInfo.devPct;
        }

        // Calculate distribution amounts
        CAmount burnAmount = (winningBid.bidAmount * burnPct) / 100;
        CAmount namespaceAmount = (winningBid.bidAmount * namespacePct) / 100;
        CAmount nodeAmount = (winningBid.bidAmount * nodePct) / 100;
        CAmount devAmount = (winningBid.bidAmount * devPct) / 100;

        // Log distribution (in a real implementation, this would create actual transactions)
        LogPrintf("Auction proceeds distribution for %s.%s:\n", auction.name, auction.namespaceId);
        LogPrintf("  Total amount: %s RDD\n", FormatMoney(winningBid.bidAmount));
        LogPrintf("  Burn pool: %s RDD (%d%%)\n", FormatMoney(burnAmount), burnPct);
        LogPrintf("  Namespace owner: %s RDD (%d%%)\n", FormatMoney(namespaceAmount), namespacePct);
        LogPrintf("  Node operators: %s RDD (%d%%)\n", FormatMoney(nodeAmount), nodePct);
        LogPrintf("  Development fund: %s RDD (%d%%)\n", FormatMoney(devAmount), devPct);
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::DistributeAuctionProceeds: Exception: %s\n", e.what());
    }
}

bool AuctionManager::RefundLosingBids(const uint256& auctionId, const uint256& winningBidId) {
    LogPrint(BCLog::REDDID, "Refunding losing bids for auction %s\n", auctionId.ToString());

    try {
        auto it = auctionBids.find(auctionId);
        if (it == auctionBids.end()) {
            LogPrintf("Error: AuctionManager::RefundLosingBids: No bids found for auction: %s\n", auctionId.ToString());
            return false;
        }

        for (auto& bid : it->second) {
            if (bid.bidId != winningBidId && !bid.refunded) {
                // Refund would happen in a real implementation
                // For now, just mark as refunded
                bid.refunded = true;

                if (reddidDB) {
                    reddidDB->UpdateBidStatus(bid.bidId, false, true);
                }

                LogPrint(BCLog::REDDID, "Refunded bid %s: %s RDD\n", bid.bidId.ToString(), FormatMoney(bid.bidAmount));
            }
        }

        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::RefundLosingBids: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::UpdateAuctionState(uint256& auctionId, AuctionState newState) {
    LogPrint(BCLog::REDDID, "Updating auction %s state to %d\n", auctionId.ToString(), newState);

    try {
        auto it = auctions.find(auctionId);
        if (it == auctions.end()) {
            LogPrintf("Error: AuctionManager::UpdateAuctionState: Auction not found: %s\n", auctionId.ToString());
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
        if (reddidDB) {
            return reddidDB->WriteAuction(auctionId, it->second);
        }

        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::UpdateAuctionState: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::ExtendAuctionIfNeeded(AuctionInfo& auction, int64_t bidTime) {
    LogPrint(BCLog::REDDID, "Checking if auction %s needs extension\n", auction.auctionId.ToString());

    try {
        // Check if this is a late bid (less than 24 hours before end)
        int64_t timeLeft = auction.endTime - bidTime;
        if (timeLeft < 24 * 60 * 60) {  // Less than 24 hours left
            // Extend by 12 hours for namespace auctions, 6 hours for user ID auctions
            int64_t extension = auction.name.empty() ? 12 * 60 * 60 : 6 * 60 * 60;
            auction.endTime = bidTime + extension;

            LogPrint(BCLog::REDDID, "Extended auction %s by %d hours due to late bid\n",
                    auction.auctionId.ToString(), extension / 3600);
            return true;
        }

        return false;
    } catch (const std::exception& e) {
        LogPrintf("Error: AuctionManager::ExtendAuctionIfNeeded: Exception: %s\n", e.what());
        return false;
    }
}
