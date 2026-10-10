// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>

#include <chain.h>
#include <coins.h>
#include <crypto/common.h>
#include <hash.h>
#include <key.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <streams.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

AuctionManager::AuctionManager(ReddIDManager& manager)
    : m_initialized(false),
      m_running(false),
      reddIDManager(&manager),
      reddidDB(nullptr),
      reddidP2P(nullptr),
      namespaceManager(nullptr),
      chainstate(nullptr) {

    // Clear existing data
    auctions.clear();
    auctionBids.clear();
    activeAuctions.clear();
}

AuctionManager::~AuctionManager() {
    Stop();
}

bool AuctionManager::Init(ReddIDManager* manager) {
    if (m_initialized) {
        LogPrint(BCLog::REDDID, "AuctionManager already initialized\n");
        return true;
    }

    if (!manager) {
        LogPrintf("ERROR: AuctionManager::Init: Null ReddIDManager\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Initializing Auction manager\n");

    try {
        // Store the manager reference
        reddIDManager = manager;

        namespaceManager = manager->GetNamespaceManager();
        if (!namespaceManager) {
            LogPrint(BCLog::REDDID, "WARNING: AuctionManager::Init: namespaceManager not available yet\n");
        }

        reddidDB = manager->GetReddIDDB();
        if (!reddidDB) {
            LogPrint(BCLog::REDDID, "WARNING: AuctionManager::Init: reddidDB not available yet\n");
        }

        reddidP2P = manager->GetP2PManager();
        if (!reddidP2P) {
            LogPrint(BCLog::REDDID, "WARNING: AuctionManager::Init: reddidP2P not available yet\n");
        }

        chainstate = manager->GetChainState();
        if (!chainstate) {
           LogPrint(BCLog::REDDID, "WARNING: AuctionManager::Init: chainstate not available yet\n");
        }

        // Load existing data from database
        if (!Load()) {
            LogPrintf("WARNING: AuctionManager::Init: Failed to load auction data\n");
            // Continue anyway, database might be empty
        }

        m_initialized = true;
        LogPrint(BCLog::REDDID, "AuctionManager initialized successfully\n");
        return true;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: AuctionManager::Init: Exception: %s\n", e.what());
        return false;
    }
}

bool AuctionManager::Start() {
    if (!m_initialized) {
        LogPrintf("ERROR: AuctionManager::Start: Not initialized\n");
        return false;
    }

    if (m_running) {
        LogPrint(BCLog::REDDID, "AuctionManager already running\n");
        return true;
    }

    LogPrint(BCLog::REDDID, "Starting AuctionManager\n");

    m_running = true;
    return true;
}

void AuctionManager::Interrupt() {
    if (!m_running) {
        return;
    }

    LogPrint(BCLog::REDDID, "Interrupting AuctionManager manager\n");
    m_running = false;
}

bool AuctionManager::Stop() {
    if (!m_initialized) {
        return true;
    }

    LogPrintf("Stopping AuctionManager\n");

    // Mark as not running
    m_running = false;

    // Save current state
    if (!Save()) {
        LogPrintf("WARNING: AuctionManager::Stop: Failed to save auction data\n");
    }

    // Clear references
    reddIDManager = nullptr;
    reddidDB = nullptr;
    reddidP2P = nullptr;
    namespaceManager = nullptr;
    chainstate = nullptr;

    m_initialized = false;

    LogPrint(BCLog::REDDID, "AuctionManager stopped\n");
    return true;
}

bool AuctionManager::Load() {
    LogPrint(BCLog::REDDID, "Loading auction data...\n");

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

    // Load deposit locks
    std::vector<DepositLock> allLocks;
    if (reddidDB->ListAllDepositLocks(allLocks)) {
        for (const auto& lock : allLocks) {
            depositLocks[lock.utxo] = lock;
        }
    }

    // Load auction configs
    std::vector<AuctionConfig> allConfigs;
    if (reddidDB->ListAuctionConfigs(allConfigs)) {
        for (const auto& config : allConfigs) {
            auctionConfigs[config.configId] = config;
        }
    }

    // Initialize default configs if not present
    if (auctionConfigs.find("namespace") == auctionConfigs.end()) {
        auctionConfigs["namespace"] = AuctionConfig::NamespaceDefaults();
    }
    if (auctionConfigs.find("userid") == auctionConfigs.end()) {
        auctionConfigs["userid"] = AuctionConfig::UserIDDefaults();
    }
    if (auctionConfigs.find("reddid") == auctionConfigs.end()) {
        auctionConfigs["reddid"] = AuctionConfig::ReddIDDefaults();
    }

    LogPrint(BCLog::REDDID, "Loaded %d auctions (%d active), %d deposit locks, %d configs\n",
             auctions.size(), activeAuctions.size(), depositLocks.size(), auctionConfigs.size());

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

        // Save deposit locks
        for (const auto& pair : depositLocks) {
            if (!reddidDB->WriteDepositLock(pair.first, pair.second)) {
                LogPrintf("Error: Failed to save deposit lock %s:%d\n",
                         pair.first.hash.ToString(), pair.first.n);
                return false;
            }
        }

        // Save auction configs
        for (const auto& pair : auctionConfigs) {
            if (!reddidDB->WriteAuctionConfig(pair.first, pair.second)) {
                LogPrintf("Error: Failed to save auction config %s\n", pair.first);
                return false;
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
           it->second.state == AUCTION_SETTLEMENT ||
           it->second.state == AUCTION_COMPLETED ||
           it->second.state == AUCTION_DEFAULTED ||
           it->second.state == AUCTION_CANCELED;
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
                            // Format: 'R' + opcode + len(1) + name + len(1) + namespace
                            LogPrint(BCLog::REDDID, "Received user ID auction creation transaction: %s\n", tx.GetHash().ToString());

                            if (vchData.size() < 5) {  // minimum: 'R'(1) + opcode(1) + len(1) + name(1) + len(1)
                                LogPrintf("ERROR: AuctionManager: OP_AUCTION_CREATE data too short\n");
                                return false;
                            }

                            // Read length-prefixed name
                            uint8_t nameLen = vchData[2];
                            if (nameLen == 0 || nameLen > MAX_REDDID_LENGTH || (size_t)(3 + nameLen) >= vchData.size()) {
                                LogPrintf("ERROR: AuctionManager: Invalid name length: %d\n", nameLen);
                                return false;
                            }
                            std::string name(vchData.begin() + 3, vchData.begin() + 3 + nameLen);

                            // Read length-prefixed namespace
                            size_t nsLenOffset = 3 + nameLen;
                            if (nsLenOffset >= vchData.size()) {
                                LogPrintf("ERROR: AuctionManager: Missing namespace length byte\n");
                                return false;
                            }
                            uint8_t nsLen = vchData[nsLenOffset];
                            if (nsLen == 0 || nsLen > MAX_NAMESPACE_LENGTH || (nsLenOffset + 1 + nsLen) > vchData.size()) {
                                LogPrintf("ERROR: AuctionManager: Invalid namespace length: %d\n", nsLen);
                                return false;
                            }
                            std::string namespaceId(vchData.begin() + nsLenOffset + 1, vchData.begin() + nsLenOffset + 1 + nsLen);

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
                                    bidAmount = static_cast<CAmount>(ReadLE64(bidAmountData.data()));
                                } else {
                                    // Default to transaction amount
                                    for (const auto& txout : tx.vout) {
                                        if (!txout.scriptPubKey.IsUnspendable()) {
                                            bidAmount = std::max(bidAmount, txout.nValue);
                                        }
                                    }
                                }

                                // Validate bid amount
                                if (bidAmount <= 0) {
                                    LogPrintf("ERROR: AuctionManager: Invalid bid amount\n");
                                    return false;
                                }

                                // Validate auction ID
                                if (auctionId.IsNull()) {
                                    LogPrintf("ERROR: AuctionManager: Null auction ID in bid\n");
                                    return false;
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

                                // Validate auction ID
                                if (auctionId.IsNull()) {
                                    LogPrintf("ERROR: AuctionManager: Null auction ID in finalize\n");
                                    return false;
                                }

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

                                // Check if this auction has deposit-backed bids
                                bool hasDeposits = false;
                                auto bidsCheck = auctionBids.find(auctionId);
                                if (bidsCheck != auctionBids.end()) {
                                    for (const auto& bid : bidsCheck->second) {
                                        if (!bid.depositUtxo.IsNull()) {
                                            hasDeposits = true;
                                            break;
                                        }
                                    }
                                }

                                if (hasDeposits) {
                                    // Use deposit settlement flow
                                    if (BeginSettlement(auctionId, tx)) {
                                        return true;
                                    }
                                } else {
                                    // Legacy flow without deposits
                                    if (FinalizeAuction(auctionId)) {
                                        return true;
                                    }
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

                                // Validate auction ID
                                if (auctionId.IsNull()) {
                                    LogPrintf("ERROR: AuctionManager: Null auction ID in cancel\n");
                                    return false;
                                }

                                // Cancel auction
                                if (CancelAuction(auctionId, sender)) {
                                    return true;
                                }
                            }
                        } else if (opCode == OP_BID_DEPOSIT) {
                            // Parse bid deposit
                            // Format: 'R'+'D'+auctionId(32)+bidAmt(8)+vout(1)+bidderPKH(20) = 64 bytes
                            LogPrint(BCLog::REDDID, "Received bid deposit transaction: %s\n", tx.GetHash().ToString());

                            if (vchData.size() < 63) {
                                LogPrintf("ERROR: AuctionManager: OP_BID_DEPOSIT data too short: %d\n", vchData.size());
                                return false;
                            }

                            uint256 auctionId;
                            memcpy(auctionId.begin(), vchData.data() + 2, 32);
                            if (auctionId.IsNull()) {
                                LogPrintf("ERROR: AuctionManager: Null auction ID in bid deposit\n");
                                return false;
                            }

                            CAmount bidAmount = static_cast<CAmount>(ReadLE64(vchData.data() + 34));
                            if (bidAmount <= 0) {
                                LogPrintf("ERROR: AuctionManager: Invalid bid amount in deposit\n");
                                return false;
                            }

                            uint8_t depositVout = vchData[42];
                            if (depositVout >= tx.vout.size()) {
                                LogPrintf("ERROR: AuctionManager: Invalid deposit vout index: %d\n", depositVout);
                                return false;
                            }

                            CKeyID bidderPKH;
                            memcpy(bidderPKH.begin(), vchData.data() + 43, 20);

                            // Validate auction exists and is ACTIVE
                            auto auctionIt = auctions.find(auctionId);
                            if (auctionIt == auctions.end()) {
                                LogPrintf("ERROR: AuctionManager: Auction not found for deposit: %s\n", auctionId.ToString());
                                return false;
                            }

                            if (auctionIt->second.state != AUCTION_ACTIVE && auctionIt->second.state != AUCTION_PENDING) {
                                LogPrintf("ERROR: AuctionManager: Auction not active for deposit\n");
                                return false;
                            }

                            // Validate deposit output pays to bidder and meets minimum percentage
                            const CTxOut& depositOut = tx.vout[depositVout];
                            CAmount depositAmount = depositOut.nValue;

                            // Get deposit percentage from config
                            std::string configId = auctionIt->second.name.empty() ? "namespace" : "userid";
                            AuctionConfig config;
                            int depositPct = 15; // default
                            if (GetAuctionConfig(configId, config)) {
                                depositPct = config.depositPercentage;
                            }

                            CAmount minDeposit = (bidAmount * depositPct) / 100;
                            if (depositAmount < minDeposit) {
                                LogPrintf("ERROR: AuctionManager: Deposit too low: %s < %s (%d%% of %s)\n",
                                         FormatMoney(depositAmount), FormatMoney(minDeposit),
                                         depositPct, FormatMoney(bidAmount));
                                return false;
                            }

                            // Verify deposit output pays to bidder
                            CTxDestination destAddr;
                            if (ExtractDestination(depositOut.scriptPubKey, destAddr)) {
                                const PKHash* pkHash = std::get_if<PKHash>(&destAddr);
                                if (!pkHash || CKeyID(uint160(*pkHash)) != bidderPKH) {
                                    LogPrintf("ERROR: AuctionManager: Deposit output not to bidder\n");
                                    return false;
                                }
                            } else {
                                LogPrintf("ERROR: AuctionManager: Cannot extract deposit destination\n");
                                return false;
                            }

                            // Place bid (reuses existing bid logic)
                            uint256 bidId;
                            if (!PlaceBid(auctionId, bidderPKH, bidAmount, bidId)) {
                                return false;
                            }

                            // Create deposit lock for the deposit UTXO
                            COutPoint depositUtxo(tx.GetHash(), depositVout);
                            int lockHeight = nHeight;
                            if (!CreateDepositLock(depositUtxo, auctionId, bidId, bidderPKH, depositAmount, lockHeight)) {
                                return false;
                            }

                            // Update BidInfo with deposit fields
                            auto& bids = auctionBids[auctionId];
                            for (auto& bid : bids) {
                                if (bid.bidId == bidId) {
                                    bid.depositUtxo = depositUtxo;
                                    bid.depositAmount = depositAmount;
                                    bid.depositLockHeight = lockHeight;
                                    bid.depositState = DEPOSIT_LOCKED_ACTIVE;
                                    if (reddidDB) reddidDB->WriteBid(bid.bidId, bid);
                                    break;
                                }
                            }

                            // Update auction totalDepositsLocked
                            auctionIt->second.totalDepositsLocked += depositAmount;
                            if (reddidDB) reddidDB->WriteAuction(auctionId, auctionIt->second);

                            return true;
                        } else if (opCode == OP_AUCTION_SETTLE) {
                            // Parse settlement
                            // Format: 'R'+'S'+auctionId(32)+bidId(32) = 67 bytes total (66 payload)
                            LogPrint(BCLog::REDDID, "Received auction settle transaction: %s\n", tx.GetHash().ToString());

                            if (vchData.size() < 66) {
                                LogPrintf("ERROR: AuctionManager: OP_AUCTION_SETTLE data too short\n");
                                return false;
                            }

                            uint256 auctionId;
                            memcpy(auctionId.begin(), vchData.data() + 2, 32);
                            uint256 bidId;
                            memcpy(bidId.begin(), vchData.data() + 34, 32);

                            if (auctionId.IsNull() || bidId.IsNull()) {
                                LogPrintf("ERROR: AuctionManager: Null ID in settle\n");
                                return false;
                            }

                            if (CompleteSettlement(auctionId, bidId, tx)) {
                                return true;
                            }
                        } else if (opCode == OP_AUCTION_DEFAULT) {
                            // Parse default
                            // Format: 'R'+'X'+auctionId(32)+defaultedBidId(32) = 66 bytes payload
                            LogPrint(BCLog::REDDID, "Received auction default transaction: %s\n", tx.GetHash().ToString());

                            if (vchData.size() < 66) {
                                LogPrintf("ERROR: AuctionManager: OP_AUCTION_DEFAULT data too short\n");
                                return false;
                            }

                            uint256 auctionId;
                            memcpy(auctionId.begin(), vchData.data() + 2, 32);
                            uint256 defaultedBidId;
                            memcpy(defaultedBidId.begin(), vchData.data() + 34, 32);

                            if (auctionId.IsNull() || defaultedBidId.IsNull()) {
                                LogPrintf("ERROR: AuctionManager: Null ID in default\n");
                                return false;
                            }

                            if (ProcessDefault(auctionId, defaultedBidId)) {
                                return true;
                            }
                        } else if (opCode == OP_DEPOSIT_FORFEIT) {
                            // Parse deposit forfeit
                            // Format: 'R'+'F'+auctionId(32)+bidId(32) = 66 bytes payload
                            LogPrint(BCLog::REDDID, "Received deposit forfeit transaction: %s\n", tx.GetHash().ToString());

                            if (vchData.size() < 66) {
                                LogPrintf("ERROR: AuctionManager: OP_DEPOSIT_FORFEIT data too short\n");
                                return false;
                            }

                            uint256 auctionId;
                            memcpy(auctionId.begin(), vchData.data() + 2, 32);
                            uint256 bidId;
                            memcpy(bidId.begin(), vchData.data() + 34, 32);

                            if (auctionId.IsNull() || bidId.IsNull()) {
                                LogPrintf("ERROR: AuctionManager: Null ID in forfeit\n");
                                return false;
                            }

                            // Find the bid and verify it's forfeited
                            auto auctionBidsIt = auctionBids.find(auctionId);
                            if (auctionBidsIt != auctionBids.end()) {
                                for (auto& bid : auctionBidsIt->second) {
                                    if (bid.bidId == bidId && bid.depositState == DEPOSIT_FORFEITED) {
                                        // Clear the deposit lock
                                        if (!bid.depositUtxo.IsNull()) {
                                            auto lockIt = depositLocks.find(bid.depositUtxo);
                                            if (lockIt != depositLocks.end()) {
                                                if (reddidDB) reddidDB->EraseDepositLock(bid.depositUtxo);
                                                depositLocks.erase(lockIt);
                                            }
                                        }
                                        LogPrint(BCLog::REDDID, "Deposit forfeit processed for bid %s\n",
                                                 bidId.ToString());
                                        return true;
                                    }
                                }
                            }
                            LogPrintf("ERROR: AuctionManager: Bid not found or not forfeited: %s\n",
                                     bidId.ToString());
                            return false;
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

void AuctionManager::CheckExpiredAuctions() {
    ProcessExpiredAuctions(GetTime());
    int currentHeight = chainstate ? chainstate->m_chain.Height() : 0;
    if (currentHeight > 0) {
        CheckSettlementDeadlines(currentHeight);
    }
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
            if (auction.name.size() < static_cast<size_t>(namespaceInfo.minLength) || auction.name.size() > static_cast<size_t>(namespaceInfo.maxLength)) {
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

// ============================================================================
// Deposit Lock Operations
// ============================================================================

bool AuctionManager::IsDepositLocked(const COutPoint& utxo) const {
    auto it = depositLocks.find(utxo);
    if (it == depositLocks.end()) return false;
    return it->second.state == DEPOSIT_LOCKED_ACTIVE;
}

bool AuctionManager::GetDepositLock(const COutPoint& utxo, DepositLock& lock) const {
    auto it = depositLocks.find(utxo);
    if (it == depositLocks.end()) return false;
    lock = it->second;
    return true;
}

bool AuctionManager::CreateDepositLock(const COutPoint& utxo, const uint256& auctionId,
                                       const uint256& bidId, const CKeyID& bidder,
                                       CAmount amount, int lockHeight) {
    if (depositLocks.find(utxo) != depositLocks.end()) {
        LogPrintf("Error: AuctionManager::CreateDepositLock: UTXO already locked: %s:%d\n",
                 utxo.hash.ToString(), utxo.n);
        return false;
    }

    DepositLock lock;
    lock.utxo = utxo;
    lock.auctionId = auctionId;
    lock.bidId = bidId;
    lock.bidder = bidder;
    lock.amount = amount;
    lock.lockHeight = lockHeight;
    lock.releaseHeight = 0;
    lock.state = DEPOSIT_LOCKED_ACTIVE;

    depositLocks[utxo] = lock;

    if (reddidDB) {
        reddidDB->WriteDepositLock(utxo, lock);
    }

    LogPrint(BCLog::REDDID, "Created deposit lock: %s:%d amount=%s auction=%s\n",
             utxo.hash.ToString(), utxo.n, FormatMoney(amount), auctionId.ToString());
    return true;
}

bool AuctionManager::UpdateDepositLock(const COutPoint& utxo, DepositState newState) {
    auto it = depositLocks.find(utxo);
    if (it == depositLocks.end()) {
        LogPrintf("Error: AuctionManager::UpdateDepositLock: Lock not found: %s:%d\n",
                 utxo.hash.ToString(), utxo.n);
        return false;
    }

    it->second.state = newState;
    if (newState != DEPOSIT_LOCKED_ACTIVE) {
        it->second.releaseHeight = chainstate ? chainstate->m_chain.Height() : 0;
    }

    if (reddidDB) {
        reddidDB->WriteDepositLock(utxo, it->second);
    }

    LogPrint(BCLog::REDDID, "Updated deposit lock %s:%d to state %d\n",
             utxo.hash.ToString(), utxo.n, newState);
    return true;
}

bool AuctionManager::ReleaseLosingDeposits(const uint256& auctionId, const uint256& winningBidId) {
    LogPrint(BCLog::REDDID, "Releasing losing deposits for auction %s\n", auctionId.ToString());

    auto bidsIt = auctionBids.find(auctionId);
    if (bidsIt == auctionBids.end()) {
        LogPrintf("Error: AuctionManager::ReleaseLosingDeposits: No bids found for auction: %s\n",
                 auctionId.ToString());
        return false;
    }

    for (auto& bid : bidsIt->second) {
        if (bid.bidId != winningBidId && !bid.refunded && !bid.depositUtxo.IsNull()) {
            // Release the deposit lock - coins never left bidder's address
            UpdateDepositLock(bid.depositUtxo, DEPOSIT_RELEASED_LOST);
            bid.refunded = true;
            bid.depositState = DEPOSIT_RELEASED_LOST;

            if (reddidDB) {
                reddidDB->WriteBid(bid.bidId, bid);
            }

            LogPrint(BCLog::REDDID, "Released losing deposit for bid %s: %s RDD\n",
                    bid.bidId.ToString(), FormatMoney(bid.depositAmount));
        }
    }

    return true;
}

std::vector<DepositLock> AuctionManager::GetDepositLocksForAuction(const uint256& auctionId) const {
    std::vector<DepositLock> result;
    for (const auto& pair : depositLocks) {
        if (pair.second.auctionId == auctionId) {
            result.push_back(pair.second);
        }
    }
    return result;
}

// ============================================================================
// Reputation Statistics
// ============================================================================

bool AuctionManager::GetBidderStats(const CKeyID& bidder, BidderStats& stats) const {
    stats = BidderStats();

    // Track unique auctions this bidder participated in
    std::set<uint256> participatedAuctions;

    // Scan all bids across all auctions
    for (const auto& [auctionId, bids] : auctionBids) {
        for (const auto& bid : bids) {
            if (bid.bidder == bidder) {
                stats.totalBids++;
                participatedAuctions.insert(auctionId);

                if (bid.isWinner) {
                    stats.auctionWins++;
                }
            }
        }
    }

    stats.auctionParticipations = static_cast<int>(participatedAuctions.size());

    // Count deposit states
    for (const auto& entry : depositLocks) {
        const DepositLock& lock = entry.second;
        if (lock.bidder == bidder) {
            stats.depositsTotal++;
            switch (lock.state) {
                case DEPOSIT_RELEASED_LOST:
                case DEPOSIT_CONSUMED_WON:
                    stats.depositsCompleted++;
                    break;
                case DEPOSIT_FORFEITED:
                    stats.depositsForfeit++;
                    break;
                default:
                    break;
            }
        }
    }

    // Count auctions where bidder was winner but auction defaulted
    for (const auto& entry : auctions) {
        const AuctionInfo& auction = entry.second;
        if (auction.state == AUCTION_DEFAULTED && auction.currentBidder == bidder) {
            stats.auctionDefaults++;
        }
    }

    return true;
}

// ============================================================================
// Auction Config Operations
// ============================================================================

bool AuctionManager::GetAuctionConfig(const std::string& configId, AuctionConfig& config) const {
    auto it = auctionConfigs.find(configId);
    if (it == auctionConfigs.end()) return false;
    config = it->second;
    return true;
}

bool AuctionManager::SetAuctionConfig(const AuctionConfig& config) {
    auctionConfigs[config.configId] = config;
    if (reddidDB) {
        reddidDB->WriteAuctionConfig(config.configId, config);
    }
    return true;
}

// ============================================================================
// UTXO Lock Enforcement
// ============================================================================

bool AuctionManager::CheckDepositLocks(const CTransaction& tx) const {
    for (const auto& txin : tx.vin) {
        auto it = depositLocks.find(txin.prevout);
        if (it == depositLocks.end()) continue;

        const DepositLock& lock = it->second;

        switch (lock.state) {
            case DEPOSIT_LOCKED_ACTIVE:
                // Only allow if this is a valid settlement TX
                if (!IsValidSettlementTx(tx, txin.prevout)) {
                    LogPrint(BCLog::REDDID, "CheckDepositLocks: Blocked spend of active deposit %s:%d\n",
                             txin.prevout.hash.ToString(), txin.prevout.n);
                    return false;
                }
                break;
            case DEPOSIT_FORFEITED:
                // Only allow if this is a valid forfeit TX (sends to community fund)
                if (!IsValidForfeitTx(tx, txin.prevout)) {
                    LogPrint(BCLog::REDDID, "CheckDepositLocks: Blocked invalid forfeit spend %s:%d\n",
                             txin.prevout.hash.ToString(), txin.prevout.n);
                    return false;
                }
                break;
            case DEPOSIT_RELEASED_LOST:
            case DEPOSIT_CONSUMED_WON:
                // These are unlocked, allow spending
                break;
        }
    }
    return true;
}

bool AuctionManager::IsValidSettlementTx(const CTransaction& tx, const COutPoint& lockedUtxo) const {
    // Check if the TX has an OP_RETURN with OP_AUCTION_SETTLE matching the locked deposit's auction
    auto lockIt = depositLocks.find(lockedUtxo);
    if (lockIt == depositLocks.end()) return false;

    const DepositLock& lock = lockIt->second;

    for (const auto& txout : tx.vout) {
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
            std::vector<unsigned char> vchData;
            opcodetype opcode;
            CScript::const_iterator pc = txout.scriptPubKey.begin() + 1;
            if (txout.scriptPubKey.GetOp(pc, opcode, vchData) &&
                vchData.size() >= 34 && vchData[0] == 'R' && vchData[1] == OP_AUCTION_SETTLE) {
                // Extract auction ID from settlement TX
                uint256 txAuctionId;
                memcpy(txAuctionId.begin(), vchData.data() + 2, 32);
                if (txAuctionId == lock.auctionId) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool AuctionManager::IsValidForfeitTx(const CTransaction& tx, const COutPoint& lockedUtxo) const {
    auto lockIt = depositLocks.find(lockedUtxo);
    if (lockIt == depositLocks.end()) return false;

    const DepositLock& lock = lockIt->second;

    // Get the community fund address from auction config
    std::string communityFundAddr;
    // Determine config type based on auction
    auto auctionIt = auctions.find(lock.auctionId);
    if (auctionIt != auctions.end()) {
        std::string configId = auctionIt->second.name.empty() ? "namespace" : "userid";
        auto configIt = auctionConfigs.find(configId);
        if (configIt != auctionConfigs.end()) {
            communityFundAddr = configIt->second.communityFundAddress;
        }
    }

    // If no community fund address is configured, allow the forfeit
    if (communityFundAddr.empty()) {
        return true;
    }

    // Check that TX has an OP_RETURN with OP_DEPOSIT_FORFEIT
    bool hasForfeitOpReturn = false;
    for (const auto& txout : tx.vout) {
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
            std::vector<unsigned char> vchData;
            opcodetype opcode;
            CScript::const_iterator pc = txout.scriptPubKey.begin() + 1;
            if (txout.scriptPubKey.GetOp(pc, opcode, vchData) &&
                vchData.size() >= 2 && vchData[0] == 'R' && vchData[1] == OP_DEPOSIT_FORFEIT) {
                hasForfeitOpReturn = true;
                break;
            }
        }
    }

    return hasForfeitOpReturn;
}

// ============================================================================
// Settlement Operations
// ============================================================================

bool AuctionManager::BeginSettlement(const uint256& auctionId, const CTransaction& tx) {
    LogPrint(BCLog::REDDID, "Beginning settlement for auction %s\n", auctionId.ToString());

    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
        LogPrintf("Error: AuctionManager::BeginSettlement: Auction not found: %s\n", auctionId.ToString());
        return false;
    }

    AuctionInfo& auction = auctionIt->second;

    // Must be ACTIVE or ENDED to begin settlement
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
        LogPrintf("Error: AuctionManager::BeginSettlement: Auction not active/ended: state=%d\n", auction.state);
        return false;
    }

    // Check if auction has ended
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime) {
        LogPrintf("Error: AuctionManager::BeginSettlement: Auction not ended yet\n");
        return false;
    }

    // Check if there are bids
    if (auction.currentBid == 0) {
        auction.state = AUCTION_CANCELED;
        activeAuctions.erase(auctionId);
        if (reddidDB) reddidDB->WriteAuction(auctionId, auction);
        LogPrint(BCLog::REDDID, "Auction %s canceled due to no bids\n", auctionId.ToString());
        return true;
    }

    // Find winning bid (highest with confirmed deposit)
    auto bidsIt = auctionBids.find(auctionId);
    if (bidsIt == auctionBids.end() || bidsIt->second.empty()) {
        LogPrintf("Error: AuctionManager::BeginSettlement: No bids found\n");
        return false;
    }

    BidInfo winningBid;
    CAmount highestBid = 0;
    for (const auto& bid : bidsIt->second) {
        if (bid.bidAmount > highestBid && !bid.refunded) {
            // If deposits are in use, verify the bid has a deposit
            if (!bid.depositUtxo.IsNull()) {
                auto lockIt = depositLocks.find(bid.depositUtxo);
                if (lockIt != depositLocks.end() && lockIt->second.state == DEPOSIT_LOCKED_ACTIVE) {
                    highestBid = bid.bidAmount;
                    winningBid = bid;
                }
            } else {
                // Legacy bid without deposit
                highestBid = bid.bidAmount;
                winningBid = bid;
            }
        }
    }

    if (highestBid == 0) {
        LogPrintf("Error: AuctionManager::BeginSettlement: No valid bids found\n");
        return false;
    }

    // Mark winning bid
    for (auto& bid : bidsIt->second) {
        if (bid.bidId == winningBid.bidId) {
            bid.isWinner = true;
            if (reddidDB) reddidDB->UpdateBidStatus(bid.bidId, true, false);
        }
    }

    // Release all losing deposits
    ReleaseLosingDeposits(auctionId, winningBid.bidId);

    // Get settlement window from config
    std::string configId = auction.name.empty() ? "namespace" : "userid";
    AuctionConfig config;
    int settlementWindow = 4320; // default ~3 days
    if (GetAuctionConfig(configId, config)) {
        settlementWindow = config.settlementWindowBlocks;
    }

    // Set settlement deadline
    int currentHeight = chainstate ? chainstate->m_chain.Height() : 0;
    auction.settlementDeadlineHeight = currentHeight + settlementWindow;
    auction.finalizeTxid = tx.GetHash();
    auction.state = AUCTION_SETTLEMENT;
    activeAuctions.erase(auctionId);

    if (reddidDB) reddidDB->WriteAuction(auctionId, auction);

    LogPrint(BCLog::REDDID, "Settlement begun for auction %s, winner=%s, deadline height=%d\n",
             auctionId.ToString(), winningBid.bidder.ToString(), auction.settlementDeadlineHeight);
    return true;
}

bool AuctionManager::CompleteSettlement(const uint256& auctionId, const uint256& bidId, const CTransaction& tx) {
    LogPrint(BCLog::REDDID, "Completing settlement for auction %s\n", auctionId.ToString());

    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
        LogPrintf("Error: AuctionManager::CompleteSettlement: Auction not found\n");
        return false;
    }

    AuctionInfo& auction = auctionIt->second;

    // Must be in SETTLEMENT state
    if (auction.state != AUCTION_SETTLEMENT) {
        LogPrintf("Error: AuctionManager::CompleteSettlement: Auction not in settlement: state=%d\n", auction.state);
        return false;
    }

    // Check deadline
    int currentHeight = chainstate ? chainstate->m_chain.Height() : 0;
    if (auction.settlementDeadlineHeight > 0 && currentHeight > auction.settlementDeadlineHeight) {
        LogPrintf("Error: AuctionManager::CompleteSettlement: Past settlement deadline\n");
        return false;
    }

    // Find the winning bid
    BidInfo winningBid;
    bool found = false;
    auto bidsIt = auctionBids.find(auctionId);
    if (bidsIt != auctionBids.end()) {
        for (const auto& bid : bidsIt->second) {
            if (bid.bidId == bidId && bid.isWinner) {
                winningBid = bid;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        LogPrintf("Error: AuctionManager::CompleteSettlement: Winning bid not found: %s\n", bidId.ToString());
        return false;
    }

    // Verify the TX includes the winner's locked deposit UTXO as input
    if (!winningBid.depositUtxo.IsNull()) {
        bool depositSpent = false;
        for (const auto& txin : tx.vin) {
            if (txin.prevout == winningBid.depositUtxo) {
                depositSpent = true;
                break;
            }
        }
        if (!depositSpent) {
            LogPrintf("Error: AuctionManager::CompleteSettlement: Winner's deposit UTXO not spent in TX\n");
            return false;
        }

        // Mark deposit as consumed
        UpdateDepositLock(winningBid.depositUtxo, DEPOSIT_CONSUMED_WON);
    }

    // Register the resource
    if (auction.name.empty()) {
        // Namespace auction - delegate to NamespaceManager
        if (namespaceManager) {
            // The namespace creation is handled by the existing finalization code path
            // We call FinalizeNamespaceAuction which creates the namespace
            // But since we already did bid selection, we need to create it directly
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
            nsInfo.lastUpdated = GetTime();
            nsInfo.expiration = GetTime() + (365 * 24 * 60 * 60);

            if (!namespaceManager->CreateNamespace(nsInfo)) {
                LogPrintf("Error: AuctionManager::CompleteSettlement: Failed to create namespace\n");
                return false;
            }
        }
    } else {
        // User ID auction - register directly
        UserIDInfo userID;
        userID.name = auction.name;
        userID.namespaceId = auction.namespaceId;
        userID.owner = winningBid.bidder;
        userID.registrationTime = GetTime();

        NamespaceInfo namespaceInfo;
        if (namespaceManager && namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
            userID.expirationTime = GetTime() + (namespaceInfo.renewalPeriod * 24 * 60 * 60);
        } else {
            userID.expirationTime = GetTime() + (365 * 24 * 60 * 60);
        }

        userID.lastTransaction = GetTime();
        userID.transactionCount = 1;

        if (reddidDB && !reddidDB->WriteUserID(auction.name, auction.namespaceId, userID)) {
            LogPrintf("Error: AuctionManager::CompleteSettlement: Failed to create user ID\n");
            return false;
        }
    }

    // Update winning bid deposit state
    for (auto& bid : bidsIt->second) {
        if (bid.bidId == bidId) {
            bid.depositState = DEPOSIT_CONSUMED_WON;
            if (reddidDB) reddidDB->WriteBid(bid.bidId, bid);
            break;
        }
    }

    // Transition to COMPLETED
    auction.state = AUCTION_COMPLETED;
    auction.settleTxid = tx.GetHash();
    if (reddidDB) reddidDB->WriteAuction(auctionId, auction);

    LogPrint(BCLog::REDDID, "Settlement completed for auction %s, resource registered to %s\n",
             auctionId.ToString(), winningBid.bidder.ToString());
    return true;
}

bool AuctionManager::ProcessDefault(const uint256& auctionId, const uint256& defaultedBidId) {
    LogPrint(BCLog::REDDID, "Processing default for auction %s\n", auctionId.ToString());

    auto auctionIt = auctions.find(auctionId);
    if (auctionIt == auctions.end()) {
        LogPrintf("Error: AuctionManager::ProcessDefault: Auction not found\n");
        return false;
    }

    AuctionInfo& auction = auctionIt->second;

    // Must be in SETTLEMENT state
    if (auction.state != AUCTION_SETTLEMENT) {
        LogPrintf("Error: AuctionManager::ProcessDefault: Auction not in settlement: state=%d\n", auction.state);
        return false;
    }

    // Must be past deadline
    int currentHeight = chainstate ? chainstate->m_chain.Height() : 0;
    if (auction.settlementDeadlineHeight > 0 && currentHeight <= auction.settlementDeadlineHeight) {
        LogPrintf("Error: AuctionManager::ProcessDefault: Not past settlement deadline yet\n");
        return false;
    }

    // Find the winning bid
    BidInfo winningBid;
    bool found = false;
    auto bidsIt = auctionBids.find(auctionId);
    if (bidsIt != auctionBids.end()) {
        for (auto& bid : bidsIt->second) {
            if (bid.bidId == defaultedBidId && bid.isWinner) {
                winningBid = bid;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        LogPrintf("Error: AuctionManager::ProcessDefault: Winning bid not found: %s\n",
                 defaultedBidId.ToString());
        return false;
    }

    // Mark deposit as forfeited
    if (!winningBid.depositUtxo.IsNull()) {
        UpdateDepositLock(winningBid.depositUtxo, DEPOSIT_FORFEITED);
    }

    // Update bid state
    for (auto& bid : bidsIt->second) {
        if (bid.bidId == defaultedBidId) {
            bid.depositState = DEPOSIT_FORFEITED;
            if (reddidDB) reddidDB->WriteBid(bid.bidId, bid);
            break;
        }
    }

    // Transition to DEFAULTED
    auction.state = AUCTION_DEFAULTED;
    if (reddidDB) reddidDB->WriteAuction(auctionId, auction);

    // Create re-auction
    uint256 newAuctionId;
    CreateReAuction(auction, newAuctionId);

    LogPrint(BCLog::REDDID, "Default processed for auction %s, re-auction created: %s\n",
             auctionId.ToString(), newAuctionId.ToString());
    return true;
}

bool AuctionManager::CreateReAuction(const AuctionInfo& originalAuction, uint256& newAuctionId) {
    LogPrint(BCLog::REDDID, "Creating re-auction for %s.%s\n",
             originalAuction.name, originalAuction.namespaceId);

    AuctionInfo newAuction;
    newAuction.name = originalAuction.name;
    newAuction.namespaceId = originalAuction.namespaceId;
    newAuction.creator = originalAuction.creator;
    newAuction.startTime = GetTime();
    newAuction.endTime = newAuction.startTime + (7 * 24 * 60 * 60); // 7 days default
    newAuction.reservePrice = originalAuction.reservePrice;
    newAuction.state = AUCTION_PENDING;
    newAuction.type = originalAuction.type;
    newAuction.reAuctionCount = originalAuction.reAuctionCount + 1;
    newAuction.previousAuctionId = originalAuction.auctionId;

    return CreateAuction(newAuction, newAuctionId);
}

void AuctionManager::CheckSettlementDeadlines(int currentHeight) {
    for (auto& pair : auctions) {
        AuctionInfo& auction = pair.second;
        if (auction.state == AUCTION_SETTLEMENT &&
            auction.settlementDeadlineHeight > 0 &&
            currentHeight > auction.settlementDeadlineHeight) {
            LogPrint(BCLog::REDDID, "Auction %s past settlement deadline (height %d > %d)\n",
                     pair.first.ToString(), currentHeight, auction.settlementDeadlineHeight);
            // Note: actual default requires on-chain TX (OP_AUCTION_DEFAULT)
            // This just logs the condition for monitoring
        }
    }
}

bool AuctionManager::UndoTransaction(const CTransaction& tx, int nHeight) {
    if (!reddIDManager || !reddidDB) return false;

    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        if (txout.scriptPubKey.size() == 0 || txout.scriptPubKey[0] != OP_RETURN)
            continue;

        std::vector<unsigned char> vchData;
        opcodetype opcode;
        CScript::const_iterator pc = txout.scriptPubKey.begin() + 1;
        if (!txout.scriptPubKey.GetOp(pc, opcode, vchData) || vchData.size() < 3 || vchData[0] != 'R')
            continue;

        unsigned char opCode = vchData[1];

        switch (opCode) {
        case OP_AUCTION_CREATE: {
            // Undo: erase auction created by this TX
            // Parse name + namespace to find the matching auction
            if (vchData.size() < 5) break;
            uint8_t nameLen = vchData[2];
            if (nameLen == 0 || (size_t)(3 + nameLen) >= vchData.size()) break;
            std::string name(vchData.begin() + 3, vchData.begin() + 3 + nameLen);

            size_t nsLenOffset = 3 + nameLen;
            if (nsLenOffset >= vchData.size()) break;
            uint8_t nsLen = vchData[nsLenOffset];
            if (nsLen == 0 || (nsLenOffset + 1 + nsLen) > vchData.size()) break;
            std::string namespaceId(vchData.begin() + nsLenOffset + 1, vchData.begin() + nsLenOffset + 1 + nsLen);

            // Find auction matching name.namespace
            uint256 foundId;
            bool found = false;
            for (const auto& pair : auctions) {
                if (pair.second.name == name && pair.second.namespaceId == namespaceId) {
                    foundId = pair.first;
                    found = true;
                    break;
                }
            }

            if (found) {
                // Erase bids first
                auto bidsIt = auctionBids.find(foundId);
                if (bidsIt != auctionBids.end()) {
                    for (const auto& bid : bidsIt->second) {
                        reddidDB->EraseBid(bid.bidId);
                    }
                    auctionBids.erase(bidsIt);
                }
                activeAuctions.erase(foundId);
                auctions.erase(foundId);
                reddidDB->EraseAuction(foundId);
                LogPrint(BCLog::REDDID, "Undo auction create: %s for %s.%s\n",
                         foundId.ToString(), name, namespaceId);
            }
            return true;
        }

        case OP_AUCTION_BID: {
            // Undo: remove bid, revert currentBid/currentBidder
            if (vchData.size() < 42) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);
            CAmount bidAmount = static_cast<CAmount>(ReadLE64(vchData.data() + 34));

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt == auctions.end()) break;

            auto bidsIt = auctionBids.find(auctionId);
            if (bidsIt != auctionBids.end()) {
                auto& bids = bidsIt->second;
                for (auto bidIt = bids.begin(); bidIt != bids.end(); ++bidIt) {
                    if (bidIt->bidAmount == bidAmount && !bidIt->refunded) {
                        reddidDB->EraseBid(bidIt->bidId);
                        bids.erase(bidIt);
                        break;
                    }
                }

                // Scan remaining for new highest
                CAmount highestBid = 0;
                CKeyID highestBidder;
                for (const auto& bid : bids) {
                    if (bid.bidAmount > highestBid && !bid.refunded) {
                        highestBid = bid.bidAmount;
                        highestBidder = bid.bidder;
                    }
                }
                auctionIt->second.currentBid = highestBid;
                auctionIt->second.currentBidder = highestBidder;

                if (highestBid == 0 && auctionIt->second.state == AUCTION_ACTIVE) {
                    auctionIt->second.state = AUCTION_PENDING;
                    activeAuctions.erase(auctionId);
                }
            }
            reddidDB->WriteAuction(auctionId, auctionIt->second);
            LogPrint(BCLog::REDDID, "Undo auction bid: %s amount=%s\n",
                     auctionId.ToString(), FormatMoney(bidAmount));
            return true;
        }

        case OP_AUCTION_FINALIZE: {
            // Undo finalize/begin settlement
            if (vchData.size() < 34) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt == auctions.end()) break;

            AuctionInfo& auction = auctionIt->second;

            if (auction.state == AUCTION_SETTLEMENT) {
                // Undo BeginSettlement: re-lock released deposits, unmark winner
                auto bidsIt = auctionBids.find(auctionId);
                if (bidsIt != auctionBids.end()) {
                    for (auto& bid : bidsIt->second) {
                        if (bid.isWinner) {
                            bid.isWinner = false;
                            reddidDB->UpdateBidStatus(bid.bidId, false, false);
                        }
                        // Re-lock deposits that were released for losing bids
                        if (bid.depositState == DEPOSIT_RELEASED_LOST && !bid.depositUtxo.IsNull()) {
                            bid.depositState = DEPOSIT_LOCKED_ACTIVE;
                            bid.refunded = false;
                            reddidDB->WriteBid(bid.bidId, bid);

                            auto lockIt = depositLocks.find(bid.depositUtxo);
                            if (lockIt != depositLocks.end()) {
                                lockIt->second.state = DEPOSIT_LOCKED_ACTIVE;
                                lockIt->second.releaseHeight = 0;
                                reddidDB->WriteDepositLock(bid.depositUtxo, lockIt->second);
                            }
                        }
                    }
                }

                auction.state = AUCTION_ACTIVE;
                auction.settlementDeadlineHeight = 0;
                auction.finalizeTxid.SetNull();
                activeAuctions.insert(auctionId);
            } else if (auction.state == AUCTION_FINALIZED) {
                // Undo legacy finalize: unmark winner, revert state
                auto bidsIt = auctionBids.find(auctionId);
                if (bidsIt != auctionBids.end()) {
                    for (auto& bid : bidsIt->second) {
                        if (bid.isWinner) {
                            bid.isWinner = false;
                            reddidDB->UpdateBidStatus(bid.bidId, false, false);
                        }
                        if (bid.refunded) {
                            bid.refunded = false;
                            reddidDB->UpdateBidStatus(bid.bidId, false, false);
                        }
                    }
                }
                auction.state = AUCTION_ACTIVE;
                activeAuctions.insert(auctionId);
            }

            reddidDB->WriteAuction(auctionId, auction);
            LogPrint(BCLog::REDDID, "Undo auction finalize: %s -> ACTIVE\n", auctionId.ToString());
            return true;
        }

        case OP_AUCTION_CANCEL: {
            // Undo: revert CANCELED to PENDING or ACTIVE
            if (vchData.size() < 34) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt == auctions.end()) break;

            AuctionInfo& auction = auctionIt->second;
            if (auction.currentBid > 0) {
                auction.state = AUCTION_ACTIVE;
                activeAuctions.insert(auctionId);
            } else {
                auction.state = AUCTION_PENDING;
            }
            reddidDB->WriteAuction(auctionId, auction);
            LogPrint(BCLog::REDDID, "Undo auction cancel: %s -> state %d\n",
                     auctionId.ToString(), auction.state);
            return true;
        }

        case OP_BID_DEPOSIT: {
            // Undo: remove deposit lock, remove bid, decrement totalDepositsLocked
            if (vchData.size() < 63) break;

            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);
            CAmount bidAmount = static_cast<CAmount>(ReadLE64(vchData.data() + 34));
            uint8_t depositVout = vchData[42];
            CKeyID bidderPKH;
            memcpy(bidderPKH.begin(), vchData.data() + 43, 20);

            // Remove deposit lock
            COutPoint depositUtxo(tx.GetHash(), depositVout);
            auto lockIt = depositLocks.find(depositUtxo);
            CAmount depositAmount = 0;
            if (lockIt != depositLocks.end()) {
                depositAmount = lockIt->second.amount;
                reddidDB->EraseDepositLock(depositUtxo);
                depositLocks.erase(lockIt);
            }

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt != auctions.end()) {
                // Decrement total deposits
                auctionIt->second.totalDepositsLocked -= depositAmount;
                if (auctionIt->second.totalDepositsLocked < 0)
                    auctionIt->second.totalDepositsLocked = 0;

                // Remove the bid
                auto bidsIt = auctionBids.find(auctionId);
                if (bidsIt != auctionBids.end()) {
                    auto& bids = bidsIt->second;
                    for (auto bidIt = bids.begin(); bidIt != bids.end(); ++bidIt) {
                        if (bidIt->bidder == bidderPKH && bidIt->bidAmount == bidAmount) {
                            reddidDB->EraseBid(bidIt->bidId);
                            bids.erase(bidIt);
                            break;
                        }
                    }

                    // Scan remaining for new highest
                    CAmount highestBid = 0;
                    CKeyID highestBidder;
                    for (const auto& bid : bids) {
                        if (bid.bidAmount > highestBid && !bid.refunded) {
                            highestBid = bid.bidAmount;
                            highestBidder = bid.bidder;
                        }
                    }
                    auctionIt->second.currentBid = highestBid;
                    auctionIt->second.currentBidder = highestBidder;

                    if (highestBid == 0 && auctionIt->second.state == AUCTION_ACTIVE) {
                        auctionIt->second.state = AUCTION_PENDING;
                        activeAuctions.erase(auctionId);
                    }
                }
                reddidDB->WriteAuction(auctionId, auctionIt->second);
            }

            LogPrint(BCLog::REDDID, "Undo bid deposit: %s auction=%s\n",
                     depositUtxo.ToString(), auctionId.ToString());
            return true;
        }

        case OP_AUCTION_SETTLE: {
            // Undo: delete created resource, revert deposit, COMPLETED->SETTLEMENT
            if (vchData.size() < 66) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);
            uint256 bidId;
            memcpy(bidId.begin(), vchData.data() + 34, 32);

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt == auctions.end()) break;

            AuctionInfo& auction = auctionIt->second;

            // Delete the registered resource
            if (auction.name.empty()) {
                // Namespace — erase directly (bypass time check in ExpireNamespace)
                if (namespaceManager) {
                    // Remove from namespace manager's maps via DB erase
                    reddidDB->EraseNamespace(auction.namespaceId);
                }
            } else {
                // User ID
                reddidDB->EraseUserID(auction.name, auction.namespaceId);
            }

            // Revert winning bid deposit state
            auto bidsIt = auctionBids.find(auctionId);
            if (bidsIt != auctionBids.end()) {
                for (auto& bid : bidsIt->second) {
                    if (bid.bidId == bidId) {
                        bid.depositState = DEPOSIT_LOCKED_ACTIVE;
                        reddidDB->WriteBid(bid.bidId, bid);

                        // Revert deposit lock
                        if (!bid.depositUtxo.IsNull()) {
                            auto lockIt = depositLocks.find(bid.depositUtxo);
                            if (lockIt != depositLocks.end()) {
                                lockIt->second.state = DEPOSIT_LOCKED_ACTIVE;
                                lockIt->second.releaseHeight = 0;
                                reddidDB->WriteDepositLock(bid.depositUtxo, lockIt->second);
                            }
                        }
                        break;
                    }
                }
            }

            // Revert state
            auction.state = AUCTION_SETTLEMENT;
            auction.settleTxid.SetNull();
            reddidDB->WriteAuction(auctionId, auction);
            LogPrint(BCLog::REDDID, "Undo auction settle: %s -> SETTLEMENT\n", auctionId.ToString());
            return true;
        }

        case OP_AUCTION_DEFAULT: {
            // Undo: delete re-auction, revert deposit, DEFAULTED->SETTLEMENT
            if (vchData.size() < 66) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);
            uint256 defaultedBidId;
            memcpy(defaultedBidId.begin(), vchData.data() + 34, 32);

            auto auctionIt = auctions.find(auctionId);
            if (auctionIt == auctions.end()) break;

            // Find and erase the re-auction (has previousAuctionId == auctionId)
            uint256 reAuctionId;
            bool foundReAuction = false;
            for (const auto& pair : auctions) {
                if (pair.second.previousAuctionId == auctionId) {
                    reAuctionId = pair.first;
                    foundReAuction = true;
                    break;
                }
            }
            if (foundReAuction) {
                auto reAuctionBidsIt = auctionBids.find(reAuctionId);
                if (reAuctionBidsIt != auctionBids.end()) {
                    for (const auto& bid : reAuctionBidsIt->second) {
                        reddidDB->EraseBid(bid.bidId);
                    }
                    auctionBids.erase(reAuctionBidsIt);
                }
                activeAuctions.erase(reAuctionId);
                auctions.erase(reAuctionId);
                reddidDB->EraseAuction(reAuctionId);
            }

            // Revert winning bid deposit from FORFEITED to LOCKED_ACTIVE
            auto bidsIt = auctionBids.find(auctionId);
            if (bidsIt != auctionBids.end()) {
                for (auto& bid : bidsIt->second) {
                    if (bid.bidId == defaultedBidId) {
                        bid.depositState = DEPOSIT_LOCKED_ACTIVE;
                        reddidDB->WriteBid(bid.bidId, bid);

                        if (!bid.depositUtxo.IsNull()) {
                            auto lockIt = depositLocks.find(bid.depositUtxo);
                            if (lockIt != depositLocks.end()) {
                                lockIt->second.state = DEPOSIT_LOCKED_ACTIVE;
                                lockIt->second.releaseHeight = 0;
                                reddidDB->WriteDepositLock(bid.depositUtxo, lockIt->second);
                            }
                        }
                        break;
                    }
                }
            }

            // Revert state
            auctionIt->second.state = AUCTION_SETTLEMENT;
            reddidDB->WriteAuction(auctionId, auctionIt->second);
            LogPrint(BCLog::REDDID, "Undo auction default: %s -> SETTLEMENT\n", auctionId.ToString());
            return true;
        }

        case OP_DEPOSIT_FORFEIT: {
            // Undo: re-create the DepositLock with FORFEITED state
            if (vchData.size() < 66) break;
            uint256 auctionId;
            memcpy(auctionId.begin(), vchData.data() + 2, 32);
            uint256 bidId;
            memcpy(bidId.begin(), vchData.data() + 34, 32);

            // Find the bid to reconstruct lock info
            auto bidsIt = auctionBids.find(auctionId);
            if (bidsIt != auctionBids.end()) {
                for (const auto& bid : bidsIt->second) {
                    if (bid.bidId == bidId && !bid.depositUtxo.IsNull()) {
                        DepositLock lock;
                        lock.utxo = bid.depositUtxo;
                        lock.auctionId = auctionId;
                        lock.bidId = bidId;
                        lock.bidder = bid.bidder;
                        lock.amount = bid.depositAmount;
                        lock.lockHeight = bid.depositLockHeight;
                        lock.releaseHeight = bid.depositReleaseHeight;
                        lock.state = DEPOSIT_FORFEITED;

                        depositLocks[bid.depositUtxo] = lock;
                        reddidDB->WriteDepositLock(bid.depositUtxo, lock);
                        LogPrint(BCLog::REDDID, "Undo deposit forfeit: re-created lock for %s\n",
                                 bid.depositUtxo.ToString());
                        break;
                    }
                }
            }
            return true;
        }

        default:
            break;
        }
    }
    return true;
}
