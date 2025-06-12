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
#include <utility>
#include <vector>

NamespaceManager::NamespaceManager(ReddIDManager& manager)
    : m_initialized(false),
      m_running(false),
      reddIDManager(nullptr),
      reddidDB(nullptr),
      reddidP2P(nullptr),
      namespaceCacheMutex() {

    // Initialize with ReddIDManager references
    reddidDB = manager.GetReddIDDB();
    reddidP2P = manager.GetP2PManager();

    // Initialize empty containers
    namespaces.clear();
    namespaceAuctions.clear();
    namespaceAuctionBids.clear();
    namespacePricingTiers.clear();
    m_namespaceCache.clear();
    cacheAccessTimes.clear();
}

NamespaceManager::~NamespaceManager() {
    Stop();
}

bool NamespaceManager::Init(ReddIDManager* manager) {
    if (m_initialized) {
        LogPrint(BCLog::REDDID, "NamespaceManager already initialized\n");
        return true;
    }

    if (!manager) {
        LogPrintf("ERROR: NamespaceManager::Init: Null ReddIDManager\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Initializing namespace manager\n");

    try {
        // Store the manager reference
        reddIDManager = manager;

        // Get the database reference
        reddidDB = manager->GetReddIDDB();
        if (!reddidDB) {
            LogPrintf("ERROR: NamespaceManager::Init: Failed to get database\n");
            return false;
        }

        // Get the P2P manager reference
        reddidP2P = manager->GetP2PManager();
        if (!reddidP2P) {
            LogPrint(BCLog::REDDID, "WARNING: NamespaceManager::Init: P2P manager not available yet\n");
            // This is OK during initialization, P2P manager is created later
        }

        // Load existing data from database
        if (!Load()) {
            LogPrintf("WARNING: NamespaceManager::Init: Failed to load namespace data\n");
            // Continue anyway, database might be empty
        }

        m_initialized = true;
        LogPrint(BCLog::REDDID, "Namespace manager initialized successfully\n");
        return true;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::Init: Exception: %s\n", e.what());
        return false;
    }
}

bool NamespaceManager::Start() {
    if (!m_initialized) {
        LogPrintf("ERROR: NamespaceManager::Start: Not initialized\n");
        return false;
    }

    if (m_running) {
        LogPrint(BCLog::REDDID, "NamespaceManager already running\n");
        return true;
    }

    LogPrint(BCLog::REDDID, "Starting namespace manager\n");

    // Get updated P2P manager reference if we didn't have it during init
    if (!reddidP2P && reddIDManager) {
        reddidP2P = reddIDManager->GetP2PManager();
        if (reddidP2P) {
            LogPrint(BCLog::REDDID, "NamespaceManager::Start: P2P manager now available\n");
        }
    }

    m_running = true;
    return true;
}

void NamespaceManager::Interrupt() {
    if (!m_running) {
        return;
    }

    LogPrint(BCLog::REDDID, "Interrupting namespace manager\n");
    m_running = false;
}

bool NamespaceManager::Stop() {
    if (!m_initialized) {
        return true;
    }

    LogPrint(BCLog::REDDID, "Stopping namespace manager\n");

    // Mark as not running
    m_running = false;

    // Save current state
    if (!Save()) {
        LogPrintf("WARNING: NamespaceManager::Stop: Failed to save namespace data\n");
    }

    // Clear caches
    {
        std::lock_guard<std::mutex> lock(namespaceCacheMutex);
        m_namespaceCache.clear();
        cacheAccessTimes.clear();
    }

    // Clear references
    reddidP2P = nullptr;
    reddidDB = nullptr;
    reddIDManager = nullptr;

    m_initialized = false;

    LogPrint(BCLog::REDDID, "Namespace manager stopped\n");
    return true;
}

bool NamespaceManager::Load() {
    LogPrintf("Loading namespace data...\n");

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
    // Validate parameters
    if (namespaceInfo.id.empty()) {
        LogPrintf("ERROR: NamespaceManager::UpdateNamespace: Empty namespace ID\n");
        return false;
    }

    // Check if namespace exists
    auto it = namespaces.find(namespaceInfo.id);
    if (it == namespaces.end()) {
        LogPrintf("ERROR: NamespaceManager::UpdateNamespace: Namespace not found: %s\n", namespaceInfo.id);
        return false;
    }

    // Check if owner matches
    if (it->second.owner != namespaceInfo.owner) {
        LogPrintf("ERROR: NamespaceManager::UpdateNamespace: Not authorized to update namespace: %s\n", namespaceInfo.id);
        return false;
    }

    // Validate namespace configuration
    if (!ValidateNamespaceConfig(namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::UpdateNamespace: Invalid namespace configuration\n");
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
    if (!reddidDB->WriteNamespace(nsInfo.id, nsInfo)) {
        LogPrintf("ERROR: NamespaceManager::UpdateNamespace: Failed to write namespace: %s\n", nsInfo.id);

        // Revert memory change on database failure
        namespaces[nsInfo.id] = it->second;
        return false;
    }

    // Announce update via P2P if available
    if (reddidP2P != nullptr) {
        try {
            // Send namespace config update
            if (!reddidP2P->SendNamespaceConfig(nullptr, nsInfo.id)) {
                LogPrintf("WARNING: NamespaceManager::UpdateNamespace: Failed to announce namespace update: %s\n", nsInfo.id);
                // Don't fail the operation on P2P failure
            }
        } catch (const std::exception& e) {
            LogPrintf("WARNING: NamespaceManager::UpdateNamespace: Exception in P2P announcement: %s\n", e.what());
            // Don't fail the operation on P2P failure
        }
    }

    LogPrint(BCLog::REDDID, "Updated namespace: %s\n", nsInfo.id);
    return true;
}

bool NamespaceManager::RenewNamespace(const std::string& namespaceId, const CKeyID& owner, int renewalDays) {
    // Validate parameters
    if (namespaceId.empty()) {
        LogPrintf("ERROR: NamespaceManager::RenewNamespace: Empty namespace ID\n");
        return false;
    }

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::RenewNamespace: Namespace not found: %s\n", namespaceId);
        return false;
    }

    // Validate ownership
    if (namespaceInfo.owner != owner) {
        LogPrintf("ERROR: NamespaceManager::RenewNamespace: Not authorized to renew namespace: %s\n", namespaceId);
        return false;
    }

    // Calculate renewal fee
    CAmount renewalFee = CalculateRenewalFee(namespaceId);

    // Update expiration time
    int64_t currentTime = GetTime();
    if (renewalDays <= 0) {
        renewalDays = namespaceInfo.renewalPeriod;  // Use default renewal period if not specified
    }

    if (namespaceInfo.expiration > currentTime) {
        // If not expired, add renewal period to current expiration
        namespaceInfo.expiration += renewalDays * 24 * 60 * 60;
    } else {
        // If expired, set new expiration from current time
        namespaceInfo.expiration = currentTime + renewalDays * 24 * 60 * 60;
    }

    // Update last updated timestamp
    namespaceInfo.lastUpdated = currentTime;

    // Save to database
    if (!reddidDB->WriteNamespace(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::RenewNamespace: Failed to write namespace: %s\n", namespaceId);
        return false;
    }

    LogPrint(BCLog::REDDID, "Renewed namespace: %s until %s\n",
             namespaceId, FormatISO8601DateTime(namespaceInfo.expiration));
    return true;
}

bool NamespaceManager::TransferNamespace(const std::string& namespaceId, const CKeyID& from, const CKeyID& to) {
    // Validate parameters
    if (namespaceId.empty()) {
        LogPrintf("ERROR: NamespaceManager::TransferNamespace: Empty namespace ID\n");
        return false;
    }

    if (from == to) {
        LogPrintf("ERROR: NamespaceManager::TransferNamespace: Source and destination owners are the same\n");
        return false;
    }

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::TransferNamespace: Namespace not found: %s\n", namespaceId);
        return false;
    }

    // Validate current ownership
    if (namespaceInfo.owner != from) {
        LogPrintf("ERROR: NamespaceManager::TransferNamespace: Not authorized to transfer namespace: %s\n", namespaceId);
        return false;
    }

    // Update owner
    namespaceInfo.owner = to;
    namespaceInfo.lastUpdated = GetTime();

    // Save to database
    if (!reddidDB->WriteNamespace(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::TransferNamespace: Failed to write namespace: %s\n", namespaceId);
        return false;
    }

    LogPrint(BCLog::REDDID, "Transferred namespace: %s from %s to %s\n",
             namespaceId, from.ToString(), to.ToString());
    return true;
}

bool NamespaceManager::ExpireNamespace(const std::string& namespaceId) {
    // Validate parameters
    if (namespaceId.empty()) {
        LogPrintf("ERROR: NamespaceManager::ExpireNamespace: Empty namespace ID\n");
        return false;
    }

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::ExpireNamespace: Namespace not found: %s\n", namespaceId);
        return false;
    }

    // Check if namespace has expired
    int64_t currentTime = GetTime();
    int64_t expirationWithGrace = namespaceInfo.expiration + (namespaceInfo.gracePeriod * 24 * 60 * 60);

    if (currentTime <= expirationWithGrace) {
        LogPrintf("ERROR: NamespaceManager::ExpireNamespace: Namespace has not expired: %s\n", namespaceId);
        return false;
    }

    // Remove namespace from memory
    namespaces.erase(namespaceId);

    // Remove from database
    if (!reddidDB->EraseNamespace(namespaceId)) {
        LogPrintf("ERROR: NamespaceManager::ExpireNamespace: Failed to delete namespace: %s\n", namespaceId);
        return false;
    }

    LogPrint(BCLog::REDDID, "Expired namespace: %s\n", namespaceId);
    return true;
}

bool NamespaceManager::CreateNamespaceAuction(const std::string& namespaceId, const CKeyID& creator,
                                            CAmount reservePrice, int durationDays, AuctionType type,
                                            uint256& auctionId) {
    if (!m_initialized || !m_running) {
        LogPrintf("ERROR: NamespaceManager not initialized or not running\n");
        return false;
    }

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
    auction.name = "";  // No name for namespace auctions
    auction.namespaceId = namespaceId;
    auction.creator = creator;
    auction.startTime = GetTime();
    auction.endTime = auction.startTime + (durationDays * 24 * 60 * 60);
    auction.reservePrice = reservePrice;
    auction.currentBid = 0;
    auction.depositAmount = reservePrice * 0.2;  // 20% deposit for namespaces
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
    if (timeLeft < 24 * 60 * 60) {  // Less than 24 hours left
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
    tier1.minPrice = 100000 * COIN;  // 100,000 RDD for single character
    tiers.push_back(tier1);

    PricingTier tier2;
    tier2.namespaceId = auction.namespaceId;
    tier2.minLength = 2;
    tier2.minPrice = 50000 * COIN;  // 50,000 RDD for 2 characters
    tiers.push_back(tier2);

    PricingTier tier3;
    tier3.namespaceId = auction.namespaceId;
    tier3.minLength = 3;
    tier3.minPrice = 25000 * COIN;  // 25,000 RDD for 3 characters
    tiers.push_back(tier3);

    PricingTier tier4;
    tier4.namespaceId = auction.namespaceId;
    tier4.minLength = 4;
    tier4.minPrice = 10000 * COIN;  // 10,000 RDD for 4+ characters
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
//    if (reddidP2P != nullptr) {
//        reddidP2P->AnnounceNamespaceFinalize(auctionId, winningBid.bidId, winningBid.bidAmount);
//    } else {
//        LogPrintf("Warning: P2P manager not available, auction will not be announced\n");
//    }

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
    // Validate parameters
    if (namespaceId.empty()) {
        LogPrintf("ERROR: NamespaceManager::AddPricingTier: Empty namespace ID\n");
        return false;
    }

    if (minLength <= 0) {
        LogPrintf("ERROR: NamespaceManager::AddPricingTier: Invalid minimum length: %d\n", minLength);
        return false;
    }

    if (minPrice <= 0) {
        LogPrintf("ERROR: NamespaceManager::AddPricingTier: Invalid minimum price: %s\n", FormatMoney(minPrice));
        return false;
    }

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrintf("ERROR: NamespaceManager::AddPricingTier: Namespace not found: %s\n", namespaceId);
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

            // Save to database
            if (!reddidDB->WritePricingTier(tier)) {
                LogPrintf("ERROR: NamespaceManager::AddPricingTier: Failed to write pricing tier\n");
                return false;
            }

            LogPrint(BCLog::REDDID, "Updated pricing tier for namespace %s, min length %d: %s\n",
                    namespaceId, minLength, FormatMoney(minPrice));
            return true;
        }
    }

    // Add new tier
    tiers.push_back(tier);

    // Save to database
    if (!reddidDB->WritePricingTier(tier)) {
        LogPrintf("ERROR: NamespaceManager::AddPricingTier: Failed to write pricing tier\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Added pricing tier for namespace %s, min length %d: %s\n",
            namespaceId, minLength, FormatMoney(minPrice));
    return true;
}

bool NamespaceManager::UpdatePricingTier(const std::string& namespaceId, int minLength, CAmount minPrice) {
    // This functionality is already handled in AddPricingTier
    return AddPricingTier(namespaceId, minLength, minPrice);
}

bool NamespaceManager::RemovePricingTier(const std::string& namespaceId, int minLength) {
    // Validate parameters
    if (namespaceId.empty()) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: Empty namespace ID\n");
        return false;
    }

    if (minLength <= 0) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: Invalid minimum length: %d\n", minLength);
        return false;
    }

    // Check if namespace exists
    auto nsIt = namespaces.find(namespaceId);
    if (nsIt == namespaces.end()) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: Namespace not found: %s\n", namespaceId);
        return false;
    }

    // Check if tier exists
    auto tierIt = namespacePricingTiers.find(namespaceId);
    if (tierIt == namespacePricingTiers.end()) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: No pricing tiers for namespace: %s\n", namespaceId);
        return false;
    }

    auto& tiers = tierIt->second;
    bool found = false;

    // Find tier by min length
    for (auto it = tiers.begin(); it != tiers.end(); ++it) {
        if (it->minLength == minLength) {
            // Remove from memory
            tiers.erase(it);
            found = true;
            break;
        }
    }

    if (!found) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: Tier not found for namespace %s, min length %d\n",
                 namespaceId, minLength);
        return false;
    }

    // Remove from database
    if (!reddidDB->ErasePricingTier(namespaceId, minLength)) {
        LogPrintf("ERROR: NamespaceManager::RemovePricingTier: Failed to delete pricing tier\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Removed pricing tier for namespace %s, min length %d\n",
            namespaceId, minLength);
    return true;
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
    // Try to get from cache first
    if (GetNamespaceFromCache(namespaceId, result)) {
        return true;
    }

    // If not in cache, check in-memory map
    auto it = namespaces.find(namespaceId);
    if (it != namespaces.end()) {
        result = it->second;

        // Add to cache
        AddNamespaceToCache(namespaceId, result);
        return true;
    }

    // Finally check persistent storage
    if (reddidDB->ReadNamespace(namespaceId, result)) {
        // Add to cache
        AddNamespaceToCache(namespaceId, result);
        return true;
    }

    return false;
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
        return 100000 * COIN;  // 100,000 RDD for single character
    } else if (length == 2) {
        return 50000 * COIN;  // 50,000 RDD for 2 characters
    } else if (length == 3) {
        return 25000 * COIN;  // 25,000 RDD for 3 characters
    } else {
        return 10000 * COIN;  // 10,000 RDD for 4+ characters
    }
}

CAmount NamespaceManager::CalculateRenewalFee(const std::string& namespaceId) const {
    // Base renewal fee is 25% of equivalent auction price
    CAmount auctionPrice = CalculateMinPrice(namespaceId);

    // Apply length-based discounts
    double discount = 0.0;
    size_t length = namespaceId.size();

    if (length >= 4) {
        // 10% discount for 4+ character namespaces
        discount = 0.1;
    }

    // Apply age-based discounts
    NamespaceInfo namespaceInfo;
    if (GetNamespaceInfo(namespaceId, namespaceInfo)) {
        int64_t currentTime = GetTime();
        int64_t ageSeconds = currentTime - namespaceInfo.lastUpdated;
        int64_t ageDays = ageSeconds / (24 * 60 * 60);

        if (ageDays > 365) {
            // 5% discount for namespaces older than 1 year
            discount += 0.05;
        }

        if (ageDays > 730) {
            // Additional 5% discount for namespaces older than 2 years
            discount += 0.05;
        }
    }

    // Calculate final fee with discount
    CAmount fee = auctionPrice * 0.25 * (1.0 - discount);

    // Ensure minimum fee
    CAmount minFee = 1000 * COIN;  // 1,000 RDD minimum
    return std::max(fee, minFee);
}

bool NamespaceManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    try {
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
                            if (ProcessNamespaceAuctionCreate(tx, vchData, nHeight)) {
                                return true;
                            }
                        } else if (opCode == OP_NAMESPACE_AUCTION_BID) {
                            if (ProcessNamespaceAuctionBid(tx, vchData, nHeight)) {
                                return true;
                            }
                        } else if (opCode == OP_NAMESPACE_AUCTION_FINALIZE) {
                            if (ProcessNamespaceAuctionFinalize(tx, vchData, nHeight)) {
                                return true;
                            }
                        } else if (opCode == OP_NAMESPACE_AUCTION_CANCEL) {
                            if (ProcessNamespaceAuctionCancel(tx, vchData, nHeight)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::ProcessTransaction: Exception: %s\n", e.what());
        // Continue processing - we don't want an exception in one transaction
        // to prevent processing others
    }

    return false;
}

bool NamespaceManager::ProcessNamespaceAuctionCreate(const CTransaction& tx, const std::vector<unsigned char>& data, int nHeight) {
    try {
        // Extract namespace ID from data
        // Format: 'R' + opcode + namespaceId + other_data
        if (data.size() < 4) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCreate: Invalid data format\n");
            return false;
        }

        std::string namespaceId(data.begin() + 2, data.end());

        // Validate namespace ID
        if (!ValidateNamespaceID(namespaceId)) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCreate: Invalid namespace ID: %s\n", namespaceId);
            return false;
        }

        // Check if namespace is available
        if (!IsNamespaceAvailable(namespaceId)) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCreate: Namespace not available: %s\n", namespaceId);
            return false;
        }

        // Extract transaction details
        CAmount reservePrice = 0;

        // Find inputs to determine creator
        CKeyID creator;
        bool foundCreator = false;

        for (const auto& txout : tx.vout) {
            // Skip OP_RETURN outputs
            if (txout.scriptPubKey.IsUnspendable()) {
                continue;
            }

            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address)) {
                const PKHash* pkHash = std::get_if<PKHash>(&address);
                if (pkHash) {
//                    bidder = CKeyID(*pkHash);
                    creator = CKeyID(uint160(*pkHash));
                    foundCreator = true;
                    break;
                }
            }
        }

        if (!foundCreator) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCreate: Could not determine creator\n");
            return false;
        }

        // Create auction
        uint256 auctionId;
        int durationDays = 14;  // Default
        AuctionType auctionType = AUCTION_STANDARD;

        bool success = CreateNamespaceAuction(namespaceId, creator, reservePrice, durationDays, auctionType, auctionId);

        if (success) {
            LogPrint(BCLog::REDDID, "Created namespace auction from transaction: %s, ID: %s\n",
                    tx.GetHash().ToString(), auctionId.ToString());
        }

        return success;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCreate: Exception: %s\n", e.what());
        return false;
    }
}

bool NamespaceManager::ProcessNamespaceAuctionBid(const CTransaction& tx, const std::vector<unsigned char>& data, int nHeight) {
    try {
        // Extract auction ID from data
        // Format: 'R' + opcode + auctionId + bidAmount
        if (data.size() < 36) {  // 'R' (1) + opcode (1) + auctionId (at least 32) + bidAmount (at least 2)
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Invalid data format\n");
            return false;
        }

        // Extract auction ID - the first 32 bytes after the 'R' + opcode
        uint256 auctionId;
        std::vector<unsigned char> auctionIdData(data.begin() + 2, data.begin() + 34);
        memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

        // Extract bid amount - remaining bytes represent the bid amount
        CAmount bidAmount = 0;
        if (data.size() >= 42) {  // 'R' (1) + opcode (1) + auctionId (32) + bidAmount (8)
            std::vector<unsigned char> bidAmountData(data.begin() + 34, data.begin() + 42);
            bidAmount = ReadLE64(bidAmountData.data());
        } else {
            // For backward compatibility, try to estimate bid amount from outputs
            for (const auto& txout : tx.vout) {
                if (txout.scriptPubKey.IsUnspendable()) {
                    continue;  // Skip OP_RETURN outputs
                }

                // Use the largest output as bid amount
                if (txout.nValue > bidAmount) {
                    bidAmount = txout.nValue;
                }
            }
        }

        if (bidAmount <= 0) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Invalid bid amount: %s\n",
                      FormatMoney(bidAmount));
            return false;
        }

        // Find auction
        AuctionInfo auction;
        if (!GetAuctionInfo(auctionId, auction)) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Auction not found: %s\n",
                      auctionId.ToString());
            return false;
        }

        // Validate auction state
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Auction not active: %s, state: %d\n",
                      auctionId.ToString(), auction.state);
            return false;
        }

        // Extract bidder's key ID
        CKeyID bidder;
        bool foundBidder = false;

        // For simplicity, assume the bidder is the sender of the transaction
        // We extract this from one of the transaction outputs that pays change back to the sender
        for (const auto& txout : tx.vout) {
            // Skip OP_RETURN outputs
            if (txout.scriptPubKey.IsUnspendable()) {
                continue;
            }

            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address)) {
                const PKHash* pkHash = std::get_if<PKHash>(&address);
                if (pkHash) {
//                    bidder = CKeyID(*pkHash);
                    bidder = CKeyID(uint160(*pkHash));
                    foundBidder = true;
                    break;
                }
            }
        }

        if (!foundBidder) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Could not determine bidder\n");
            return false;
        }

        // Place bid
        uint256 bidId;
        bool success = BidOnNamespaceAuction(auctionId, bidder, bidAmount, bidId);

        if (success) {
            LogPrint(BCLog::REDDID, "Processed namespace auction bid from transaction: %s, auction: %s, bid: %s\n",
                     tx.GetHash().ToString(), auctionId.ToString(), bidId.ToString());
        }

        return success;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionBid: Exception: %s\n", e.what());
        return false;
    }
}

bool NamespaceManager::ProcessNamespaceAuctionFinalize(const CTransaction& tx, const std::vector<unsigned char>& data, int nHeight) {
    try {
        // Extract auction ID from data
        // Format: 'R' + opcode + auctionId
        if (data.size() < 34) {  // 'R' (1) + opcode (1) + auctionId (at least 32)
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Invalid data format\n");
            return false;
        }

        // Extract auction ID - the first 32 bytes after the 'R' + opcode
        uint256 auctionId;
        std::vector<unsigned char> auctionIdData(data.begin() + 2, data.begin() + 34);
        memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

        // Find auction
        AuctionInfo auction;
        if (!GetAuctionInfo(auctionId, auction)) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Auction not found: %s\n",
                      auctionId.ToString());
            return false;
        }

        // Validate auction state
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Auction not active or ended: %s, state: %d\n",
                      auctionId.ToString(), auction.state);
            return false;
        }

        // Verify finalizer permissions
        // Only auction creator or highest bidder can finalize
        CKeyID finalizer;
        bool foundFinalizer = false;

        for (const auto& txout : tx.vout) {
            // Skip OP_RETURN outputs
            if (txout.scriptPubKey.IsUnspendable()) {
                continue;
            }

            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address)) {
                const PKHash* pkHash = std::get_if<PKHash>(&address);
                if (pkHash) {
//                    finalizer = CKeyID(*pkHash);
                    finalizer = CKeyID(uint160(*pkHash));
                    foundFinalizer = true;
                    break;
                }
            }
        }

        if (!foundFinalizer) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Could not determine finalizer\n");
            return false;
        }

        // Check if finalizer is authorized
        if (finalizer != auction.creator && finalizer != auction.currentBidder) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Unauthorized finalizer\n");
            return false;
        }

        // Check if auction has ended (time-based)
        int64_t currentTime = GetTime();
        if (auction.endTime > currentTime) {
            // Allow finalization by creator only if no bids received
            if (finalizer == auction.creator && auction.currentBid == 0) {
                LogPrint(BCLog::REDDID, "Early finalization by creator allowed due to no bids\n");
            } else {
                LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Auction has not ended yet: %s\n",
                          auctionId.ToString());
                return false;
            }
        }

        // Finalize auction
        bool success = FinalizeNamespaceAuction(auctionId);

        if (success) {
            LogPrint(BCLog::REDDID, "Finalized namespace auction from transaction: %s, auction: %s\n",
                     tx.GetHash().ToString(), auctionId.ToString());
        }

        return success;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionFinalize: Exception: %s\n", e.what());
        return false;
    }
}

bool NamespaceManager::ProcessNamespaceAuctionCancel(const CTransaction& tx, const std::vector<unsigned char>& data, int nHeight) {
    try {
        // Extract auction ID from data
        // Format: 'R' + opcode + auctionId
        if (data.size() < 34) {  // 'R' (1) + opcode (1) + auctionId (at least 32)
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Invalid data format\n");
            return false;
        }

        // Extract auction ID - the first 32 bytes after the 'R' + opcode
        uint256 auctionId;
        std::vector<unsigned char> auctionIdData(data.begin() + 2, data.begin() + 34);
        memcpy(auctionId.begin(), auctionIdData.data(), auctionIdData.size());

        // Find auction
        AuctionInfo auction;
        if (!GetAuctionInfo(auctionId, auction)) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Auction not found: %s\n",
                      auctionId.ToString());
            return false;
        }

        // Validate auction state
        if (auction.state != AUCTION_PENDING && auction.state != AUCTION_ACTIVE) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Auction not pending or active: %s, state: %d\n",
                      auctionId.ToString(), auction.state);
            return false;
        }

        // Identify canceller
        CKeyID canceller;
        bool foundCanceller = false;

        for (const auto& txout : tx.vout) {
            // Skip OP_RETURN outputs
            if (txout.scriptPubKey.IsUnspendable()) {
                continue;
            }

            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address)) {
                const PKHash* pkHash = std::get_if<PKHash>(&address);
                if (pkHash) {
//                    canceller = CKeyID(*pkHash);
                    canceller = CKeyID(uint160(*pkHash));
                    foundCanceller = true;
                    break;
                }
            }
        }

        if (!foundCanceller) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Could not determine canceller\n");
            return false;
        }

        // Verify cancellation permission
        if (canceller != auction.creator) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Unauthorized cancellation attempt\n");
            return false;
        }

        // Check if auction can be canceled (no bids or pending state)
        if (auction.state == AUCTION_ACTIVE && auction.currentBid > 0) {
            LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Cannot cancel auction with bids\n");
            return false;
        }

        // Cancel auction
        bool success = CancelNamespaceAuction(auctionId, canceller);

        if (success) {
            LogPrint(BCLog::REDDID, "Cancelled namespace auction from transaction: %s, auction: %s\n",
                     tx.GetHash().ToString(), auctionId.ToString());
        }

        return success;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: NamespaceManager::ProcessNamespaceAuctionCancel: Exception: %s\n", e.what());
        return false;
    }
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
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Invalid namespace ID: %s\n", config.id);
        return false;
    }

    // Validate length constraints
    if (config.minLength < 1) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Minimum length cannot be less than 1\n");
        return false;
    }

    if (config.maxLength > 64) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Maximum length cannot exceed 64\n");
        return false;
    }

    if (config.minLength > config.maxLength) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Minimum length cannot exceed maximum length\n");
        return false;
    }

    // Validate percentage distribution
    int totalPct = config.namespaceRevenuePct + config.burnPct + config.nodePct + config.devPct;
    if (totalPct != 100) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Percentage distribution must sum to 100%% (got %d%%)\n", totalPct);
        return false;
    }

    // Validate individual percentage constraints
    if (config.namespaceRevenuePct < 5 || config.namespaceRevenuePct > 15) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Namespace revenue must be between 5%% and 15%%\n");
        return false;
    }

    if (config.burnPct < 50 || config.burnPct > 80) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Burn percentage must be between 50%% and 80%%\n");
        return false;
    }

    if (config.nodePct < 5 || config.nodePct > 25) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Node percentage must be between 5%% and 25%%\n");
        return false;
    }

    if (config.devPct < 5 || config.devPct > 15) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Dev percentage must be between 5%% and 15%%\n");
        return false;
    }

    // Validate time periods
    if (config.renewalPeriod < 90 || config.renewalPeriod > 730) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Renewal period must be between 90 and 730 days\n");
        return false;
    }

    if (config.gracePeriod < 14 || config.gracePeriod > 90) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Grace period must be between 14 and 90 days\n");
        return false;
    }

    // Validate auction parameters
    if (config.minAuctionDuration < 1 || config.minAuctionDuration > 14) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Minimum auction duration must be between 1 and 14 days\n");
        return false;
    }

    if (config.maxAuctionDuration < config.minAuctionDuration || config.maxAuctionDuration > 30) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Maximum auction duration must be between minimum duration and 30 days\n");
        return false;
    }

    if (config.minBidIncrement < 1.0 || config.minBidIncrement > 20.0) {
        LogPrintf("ERROR: NamespaceManager::ValidateNamespaceConfig: Minimum bid increment must be between 1%% and 20%%\n");
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

bool NamespaceManager::GetNamespaceFromCache(const std::string& namespaceId, NamespaceInfo& info) const {
    if (namespaceId.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(namespaceCacheMutex);

    auto it = m_namespaceCache.find(namespaceId);
    if (it != m_namespaceCache.end()) {
        // Extract the NamespaceInfo from the CachedNamespace structure
        info = it->second.info;  // Use .info to access the NamespaceInfo inside CachedNamespace

        // Update access time for LRU
        it->second.cacheTime = GetTime();  // Update the timestamp in the CachedNamespace

        LogPrint(BCLog::REDDID, "Cache hit for namespace: %s\n", namespaceId);
        return true;
    }

    LogPrint(BCLog::REDDID, "Cache miss for namespace: %s\n", namespaceId);
    return false;
}

void NamespaceManager::AddNamespaceToCache(const std::string& namespaceId, const NamespaceInfo& info) const {
    std::lock_guard<std::mutex> lock(namespaceCacheMutex);

    // Create a CachedNamespace object to insert into the cache
    CachedNamespace cached;
    cached.info = info;
    cached.cacheTime = GetTime();

    // Add or update in cache
    m_namespaceCache[namespaceId] = cached;

    // Check if cache maintenance is needed
    MaintainNamespaceCache();
}

void NamespaceManager::InvalidateCache(const std::string& namespaceId) const {
    std::lock_guard<std::mutex> lock(namespaceCacheMutex);

    m_namespaceCache.erase(namespaceId);
    cacheAccessTimes.erase(namespaceId);
}

void NamespaceManager::MaintainNamespaceCache() const {
    // Note: we assume this is already called within a lock_guard

    const size_t MAX_CACHE_SIZE = 1000;  // Adjust based on your needs

    // Prune cache if it's too large
    if (m_namespaceCache.size() > MAX_CACHE_SIZE) {
        // Find least recently used entries
        std::vector<std::pair<std::string, int64_t>> accessTimes;

        // Extract namespace IDs and their cache times
        for (const auto& entry : m_namespaceCache) {
            accessTimes.push_back(std::make_pair(entry.first, entry.second.cacheTime));
        }

        // Sort by cache time (oldest first)
        std::sort(accessTimes.begin(), accessTimes.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        // Remove oldest entries until we're back to the limit
        size_t entriesToRemove = m_namespaceCache.size() - MAX_CACHE_SIZE;
        for (size_t i = 0; i < entriesToRemove; ++i) {
            const std::string& namespaceId = accessTimes[i].first;
            m_namespaceCache.erase(namespaceId);
        }

        LogPrint(BCLog::REDDID, "Pruned %zu old entries from namespace cache\n", entriesToRemove);
    }
}
