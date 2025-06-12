// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/reddid_p2p.h>
#include <id/reddid.h>
#include <id/namespace.h>
#include <id/auction.h>
#include <id/profile.h>

#include <addrman.h>
#include <chainparams.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <serialize.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

ReddIDP2PManager::ReddIDP2PManager(NodeContext& nodeIn)
    : m_initialized(false),
      m_running(false),
      reddIDManager(nullptr),
      namespaceManager(nullptr),
      profileManager(nullptr),
      auctionManager(nullptr),
      node(&nodeIn),
      connman(nullptr) {
}

ReddIDP2PManager::~ReddIDP2PManager() {
    Stop();
}

bool ReddIDP2PManager::Init(ReddIDManager* manager) {
    if (m_initialized) {
        LogPrint(BCLog::REDDID, "P2Pmanager already initialized\n");
        return true;
    }

    if (!manager) {
        LogPrintf("ERROR: P2Pmanager::Init: Null ReddIDManager\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Initializing P2Pmanager manager\n");

    try {
        // Store the manager reference
        reddIDManager = manager;

        namespaceManager = manager->GetNamespaceManager();
        if (!namespaceManager) {
            LogPrint(BCLog::REDDID, "WARNING: ReddIDP2PManager::Init: namespaceManager not available yet\n");
        }

        auctionManager = manager->GetAuctionManager();
        if (!auctionManager) {
            LogPrint(BCLog::REDDID, "WARNING: ReddIDP2PManager::Init: auctionManager not available yet\n");
        }

        profileManager = manager->GetProfileManager();
        if (!profileManager) {
            LogPrint(BCLog::REDDID, "WARNING: ReddIDP2PManager::Init: profileManager not available yet\n");
        }

        m_initialized = true;
        LogPrint(BCLog::REDDID, "ReddIDP2PManager initialized successfully\n");
        return true;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: P2Pmanager::Init: Exception: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::Start() {
    if (!m_initialized) {
        LogPrintf("ERROR: ReddIDP2PManager::Start: Not initialized\n");
        return false;
    }

    if (m_running) {
        LogPrint(BCLog::REDDID, "ReddIDP2PManager already running\n");
        return true;
    }

    LogPrint(BCLog::REDDID, "Starting P2PManager\n");

    m_running = true;
    return true;
}

void ReddIDP2PManager::Interrupt() {
    if (!m_running) {
        return;
    }

    LogPrint(BCLog::REDDID, "Interrupting P2PManager manager\n");
    m_running = false;
}

bool ReddIDP2PManager::Stop() {
    if (!m_initialized) {
        return true;
    }

    LogPrintf("Stopping P2Pmanager\n");

    // Mark as not running
    m_running = false;

    // Clear references
    reddIDManager = nullptr;

    m_initialized = false;

    LogPrint(BCLog::REDDID, "P2PManager stopped\n");
    return true;
}

bool ReddIDP2PManager::RegisterMessageHandlers() {
    if (!m_running || !node || !node->connman) {
        LogPrint(BCLog::REDDID, "Cannot register message handlers: node or connman not available\n");
        return false;
    }

    // Store the connman reference
    connman = node->connman.get();

    // Namespace operations
    messageHandlers[MSG_NAMESPACE_AUCTION_ANNOUNCE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        return this->ProcessNamespaceAuctionAnnounce(pfrom, vRecv, nTimeReceived);
    };

    messageHandlers[MSG_NAMESPACE_AUCTION_BID] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionBid msg;
        try {
            vRecv >> msg;
            return this->ProcessAuctionBid(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing auction bid: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_NAMESPACE_AUCTION_FINALIZE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionFinalize msg;
        try {
            vRecv >> msg;
            return this->ProcessAuctionFinalize(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing auction finalize: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_NAMESPACE_AUCTION_CANCEL] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        uint256 auctionId;
        try {
            vRecv >> auctionId;
            return this->ProcessNamespaceAuctionCancel(pfrom, auctionId);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing auction cancel: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_REQUEST] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        std::string namespaceId;
        try {
            vRecv >> namespaceId;
            return this->ProcessNamespaceConfigRequest(pfrom, namespaceId);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing namespace config request: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CNamespaceConfig msg;
        try {
            vRecv >> msg;
            return this->ProcessNamespaceConfigResponse(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing namespace config response: %s\n", e.what());
            return false;
        }
    };

    // User ID operations
    messageHandlers[MSG_USERID_AUCTION_ANNOUNCE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CUserIDAuctionAnnounce msg;
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionAnnounce(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing user ID auction announce: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_USERID_AUCTION_BID] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionBid msg;
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionBid(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing user ID auction bid: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_USERID_AUCTION_FINALIZE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionFinalize msg;
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionFinalize(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing user ID auction finalize: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_USERID_AUCTION_CANCEL] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        uint256 auctionId;
        try {
            vRecv >> auctionId;
            return this->ProcessUserIDAuctionCancel(pfrom, auctionId);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing user ID auction cancel: %s\n", e.what());
            return false;
        }
    };

    // ReddID operations
    messageHandlers[MSG_REDDID_AUCTION_ANNOUNCE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CUserIDAuctionAnnounce msg;  // We reuse the same message structure
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionAnnounce(pfrom, msg);  // Can reuse the same handler
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID auction announce: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_AUCTION_BID] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionBid msg;
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionBid(pfrom, msg);  // Can reuse the same handler
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID auction bid: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_AUCTION_FINALIZE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CAuctionFinalize msg;
        try {
            vRecv >> msg;
            return this->ProcessUserIDAuctionFinalize(pfrom, msg);  // Can reuse the same handler
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID auction finalize: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_PROFILE_UPDATE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDProfileUpdate msg;
        try {
            vRecv >> msg;
            return this->ProcessReddIDProfileUpdate(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID profile update: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_PROFILE_REQUEST] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDProfileRequest msg;
        try {
            vRecv >> msg;
            return this->ProcessReddIDProfileRequest(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID profile request: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_PROFILE_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDProfileUpdate msg;
        try {
            vRecv >> msg;
            return this->ProcessReddIDProfileResponse(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID profile response: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_CONNECTION] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDConnection msg;
        try {
            vRecv >> msg;
            return this->ProcessReddIDConnection(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID connection: %s\n", e.what());
            return false;
        }
    };

    messageHandlers[MSG_REDDID_REPUTATION_UPDATE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDReputationUpdate msg;
        try {
            vRecv >> msg;
            return this->ProcessReddIDReputationUpdate(pfrom, msg);
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error deserializing ReddID reputation update: %s\n", e.what());
            return false;
        }
    };

    LogPrint(BCLog::REDDID, "ReddID: Registered %d message handlers\n", messageHandlers.size());
    return true;
}

void ReddIDP2PManager::ProcessMessage(CNode& pfrom, const std::string& strCommand,
                                     CDataStream& vRecv, int64_t nTimeReceived) {
    // Early return if we're not running
    if (!m_running) {
        LogPrint(BCLog::REDDID, "Ignoring message %s - manager not running\n", strCommand);
        return;
    }

    // Check if node supports ReddID
    if (!(pfrom.nServices & NODE_REDDID)) {
        std::string serviceStr = Join(serviceFlagsToStr(pfrom.nServices), "|");
        LogPrint(BCLog::REDDID, "Ignoring ReddID message %s from incompatible peer %d (service %s)\n",
                 strCommand, pfrom.GetId(), serviceStr);
        return;
    }

    // Check if we have enough data
    if (vRecv.empty()) {
        LogPrint(BCLog::REDDID, "Empty message %s from peer %d - ignoring\n",
                 strCommand, pfrom.GetId());
        return;
    }

    // Check message size to prevent DoS
    if (vRecv.size() > MAX_REDDID_MESSAGE_SIZE) {
        LogPrint(BCLog::REDDID, "Oversized message %s (%d bytes) from peer %d - ignoring\n",
                 strCommand, vRecv.size(), pfrom.GetId());
        // Consider disconnecting persistent offenders
        return;
    }

    // Apply rate limiting
    if (!CheckMessageRate(&pfrom, strCommand)) {
        LogPrint(BCLog::REDDID, "Rate limited message %s from peer %d\n",
                 strCommand, pfrom.GetId());
        return;
    }

    LogPrint(BCLog::REDDID, "Processing message %s from peer %d\n", strCommand, pfrom.GetId());

    // Look up the appropriate handler and call it
    auto it = messageHandlers.find(strCommand);
    if (it != messageHandlers.end()) {
        try {
            // Create a copy of the stream to avoid modification
            CDataStream processingStream(vRecv);
            bool result = it->second(&pfrom, processingStream, nTimeReceived);

            if (!result) {
                LogPrint(BCLog::REDDID, "Handler returned false for message %s\n", strCommand);

                // Optional: Add node to a temporary ban list for repeated failures
                // This is a placeholder for potential future implementation
                // m_failureTracker[pfrom.GetId()]++;
            }
        } catch (const std::exception& e) {
            LogPrint(BCLog::REDDID, "Error processing message %s: %s\n", strCommand, e.what());

            // Log the message hex data for debugging in verbose mode
            if (LogAcceptCategory(BCLog::REDDID)) {
                LogPrint(BCLog::REDDID, "Message data: %s\n", HexStr(vRecv));
            }
        }
    } else {
        // No handler found
        LogPrint(BCLog::REDDID, "Unknown message type: %s\n", strCommand);
    }
}

bool ReddIDP2PManager::ProcessNamespaceAuctionAnnounce(CNode* pfrom, CDataStream& vRecv, int64_t timestamp) {
    try {
        CNamespaceAuctionAnnounce announce;
        vRecv >> announce;

        LogPrint(BCLog::REDDID, "Received namespace auction announcement for %s\n", announce.namespaceId);

        // Validate the namespace ID
        if (!namespaceManager->ValidateNamespaceID(announce.namespaceId)) {
            LogPrint(BCLog::REDDID, "Invalid namespace ID: %s\n", announce.namespaceId);
            return false;
        }

        // Check if namespace is available
        if (!namespaceManager->IsNamespaceAvailable(announce.namespaceId)) {
            LogPrint(BCLog::REDDID, "Namespace not available: %s\n", announce.namespaceId);
            return false;
        }

        // Check if we already know about this auction
        AuctionInfo auction;
        if (auctionManager->GetAuctionInfo(announce.auctionId, auction)) {
            LogPrint(BCLog::REDDID, "Already know about auction %s\n", announce.auctionId.ToString());
            return true;
        }

        // Create the auction info from the announcement
        auction.auctionId = announce.auctionId;
        auction.name = "";  // Empty for namespace auctions
        auction.namespaceId = announce.namespaceId;
        auction.startTime = announce.startTime;
        auction.endTime = announce.endTime;
        auction.reservePrice = announce.reservePrice;
        auction.type = announce.type;
        auction.state = AUCTION_PENDING;

        // We don't have creator information in the announcement yet
        // This should be added to the CNamespaceAuctionAnnounce structure
        // For now, we'll use a zero key which isn't ideal
        auction.creator = CKeyID();  // TODO(gnasher): Add creator to announcement message

        // Validate the auction parameters
        if (auction.endTime <= auction.startTime) {
            LogPrint(BCLog::REDDID, "Invalid auction times for %s\n", auction.namespaceId);
            return false;
        }

        // Verify the reserve price meets minimum requirements
        CAmount minPrice = namespaceManager->CalculateMinPrice(auction.namespaceId);
        if (auction.reservePrice < minPrice) {
            LogPrint(BCLog::REDDID, "Reserve price too low for namespace %s: %s < %s\n",
                    auction.namespaceId,
                    FormatMoney(auction.reservePrice),
                    FormatMoney(minPrice));
            return false;
        }

        // Save the auction using AuctionManager
        uint256 auctionId = auction.auctionId;
        if (!auctionManager->CreateAuction(auction, auctionId)) {
            LogPrint(BCLog::REDDID, "Failed to create auction from announcement\n");
            return false;
        }

        LogPrint(BCLog::REDDID, "Successfully created auction %s from P2P announcement\n",
                auctionId.ToString());

        // Create a data stream to serialize the message for relay
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << announce;

        // Forward the announcement to other nodes
        RelayMessage(MSG_NAMESPACE_AUCTION_ANNOUNCE, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing namespace auction announcement: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessNamespaceConfigRequest(CNode* pfrom, const std::string& namespaceId) {
    if (!pfrom || namespaceId.empty()) {
        LogPrint(BCLog::REDDID, "Invalid namespace config request: null node or empty namespace ID\n");
        return false;
    }

    LogPrint(BCLog::REDDID, "Received namespace config request for %s from node %d\n",
             namespaceId, pfrom->GetId());

    // Check if managers are initialized
    if (!namespaceManager) {
        LogPrint(BCLog::REDDID, "Namespace manager not initialized\n");
        return false;
    }

    // Check if we have this namespace
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrint(BCLog::REDDID, "Don't have info for namespace %s\n", namespaceId);
        return false;
    }

    // Send the config
    return SendNamespaceConfig(pfrom, namespaceId);
}

bool ReddIDP2PManager::ProcessNamespaceConfigResponse(CNode* pfrom, const CNamespaceConfig& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received namespace config response for %s\n", msg.config.id);

        // Basic validation
        if (msg.config.id.empty() || msg.config.id.length() > MAX_NAMESPACE_LENGTH) {
            LogPrint(BCLog::REDDID, "Invalid namespace id: %s\n", msg.config.id);
            return false;
        }

        // Validate percentage distribution
        if (msg.config.burnPct + msg.config.devPct + msg.config.namespaceRevenuePct + msg.config.nodePct != 100) {
            LogPrint(BCLog::REDDID, "Invalid percentage distribution in namespace config: %s\n", msg.config.id);
            return false;
        }

        // Check if we already have this namespace
        NamespaceInfo existingConfig;
        bool haveNamespace = namespaceManager->GetNamespaceInfo(msg.config.id, existingConfig);

        if (haveNamespace) {
            // We already have this namespace, check if it's more recent
            if (msg.config.lastUpdated <= existingConfig.lastUpdated) {
                LogPrint(BCLog::REDDID, "Already have more recent config for %s\n", msg.config.id);
                return true;
            }
        }

        // Save the config
        bool success = namespaceManager->CreateNamespace(msg.config);
        if (!success) {
            LogPrint(BCLog::REDDID, "Failed to create namespace %s\n", msg.config.id);
            return false;
        }

        // Add pricing tiers
        for (const auto& tier : msg.pricingTiers) {
            namespaceManager->AddPricingTier(msg.config.id, tier.minLength, tier.minPrice);
        }

        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the config to other nodes
        RelayMessage(MSG_NAMESPACE_CONFIG_RESPONSE, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing namespace config response: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessAuctionBid(CNode* pfrom, const CAuctionBid& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received auction bid for auction %s: %s RDD\n",
                msg.auctionId.ToString(), FormatMoney(msg.bidAmount));

        // Check if we know about this auction
        AuctionInfo auction;
        if (!auctionManager->GetAuctionInfo(msg.auctionId, auction)) {
            LogPrint(BCLog::REDDID, "Unknown auction %s for bid\n", msg.auctionId.ToString());

            // We might request auction information here, but for now, just reject the bid
            return false;
        }

        // Check if the auction is in a valid state for bids
        if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
            LogPrint(BCLog::REDDID, "Auction %s is not active or pending (state: %d)\n",
                    msg.auctionId.ToString(), auction.state);
            return false;
        }

        // Check if auction has ended
        int64_t currentTime = GetTime();
        if (auction.endTime <= currentTime) {
            LogPrint(BCLog::REDDID, "Auction %s has ended, cannot accept bids\n",
                    msg.auctionId.ToString());
            return false;
        }

        // Check if we already know about this bid
        BidInfo existingBid;
        if (auctionManager->GetBidInfo(msg.bidId, existingBid)) {
            LogPrint(BCLog::REDDID, "Already know about bid %s\n", msg.bidId.ToString());
            return true;
        }

        // Check if bid meets minimum price
        if (msg.bidAmount < auction.reservePrice) {
            LogPrint(BCLog::REDDID, "Bid amount %s is below reserve price %s\n",
                    FormatMoney(msg.bidAmount), FormatMoney(auction.reservePrice));
            return false;
        }

        // If there are existing bids, check if this bid meets minimum increment
        if (auction.currentBid > 0) {
            // Get namespace info for minimum bid increment
            double minIncrementPct = 0.05;  // Default 5%

            if (namespaceManager) {
                NamespaceInfo namespaceInfo;
                if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
                    minIncrementPct = namespaceInfo.minBidIncrement / 100.0;
                }
            }

            CAmount minBid = auction.currentBid * (1.0 + minIncrementPct);
            if (msg.bidAmount < minBid) {
                LogPrint(BCLog::REDDID, "Bid amount %s does not meet minimum increment (minimum: %s)\n",
                        FormatMoney(msg.bidAmount), FormatMoney(minBid));
                return false;
            }
        }

        // Create the bid
        BidInfo bid;
        bid.bidId = msg.bidId;
        bid.auctionId = msg.auctionId;
        bid.bidder = msg.bidder;  // Assuming the message includes bidder info
        bid.bidAmount = msg.bidAmount;
        bid.depositAmount = msg.depositAmount;
        bid.bidTime = msg.timestamp;
        bid.isWinner = false;
        bid.refunded = false;

        // Save the bid locally using AuctionManager
        uint256 bidId = bid.bidId;
        // We need to adapt this call to match the AuctionManager's PlaceBid signature
        // which doesn't take a pre-constructed BidInfo
        if (!auctionManager->PlaceBid(msg.auctionId, bid.bidder, bid.bidAmount, bidId, bid.bidTime)) {
            LogPrint(BCLog::REDDID, "Failed to process auction bid\n");
            return false;
        }

        LogPrint(BCLog::REDDID, "Successfully processed bid %s on auction %s: %s RDD\n",
                bidId.ToString(), msg.auctionId.ToString(), FormatMoney(msg.bidAmount));

        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the bid to other nodes
        RelayMessage(MSG_NAMESPACE_AUCTION_BID, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing auction bid: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessAuctionFinalize(CNode* pfrom, const CAuctionFinalize& msg) {
    LogPrintf("Received auction finalization for auction %s\n", msg.auctionId.ToString());

    // Check if we know about this auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(msg.auctionId, auction)) {
        LogPrintf("Unknown auction %s\n", msg.auctionId.ToString());
        return false;
    }

    // Check if the auction is active or ended
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
        LogPrintf("Auction %s is not active or ended\n", msg.auctionId.ToString());
        return false;
    }

    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the finalization to other nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_FINALIZE, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessNamespaceAuctionCancel(CNode* pfrom, const uint256& auctionId) {
    LogPrint(BCLog::REDDID, "Received namespace auction cancellation for auction %s\n", auctionId.ToString());

    // Check if we know about this auction
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        LogPrint(BCLog::REDDID, "Unknown auction %s\n", auctionId.ToString());
        return false;
    }

    // Check if the auction is in a state that can be canceled
    if (auction.state != AUCTION_PENDING && auction.state != AUCTION_ACTIVE) {
        LogPrint(BCLog::REDDID, "Auction %s cannot be canceled in state %d\n",
                auctionId.ToString(), auction.state);
        return false;
    }

    // If the auction has bids, it can only be canceled if it's pending
    if (auction.state == AUCTION_ACTIVE && auction.currentBid > 0) {
        LogPrint(BCLog::REDDID, "Active auction %s with bids cannot be canceled\n", auctionId.ToString());
        return false;
    }

    // Create a data stream to serialize the message for relay
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << auctionId;

    // Forward the cancellation to other nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_CANCEL, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessUserIDAuctionAnnounce(CNode* pfrom, const CUserIDAuctionAnnounce& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received user ID auction announcement for %s.%s\n", msg.name, msg.namespaceId);

        // Validate namespace exists
        NamespaceInfo namespaceInfo;
        if (!namespaceManager->GetNamespaceInfo(msg.namespaceId, namespaceInfo)) {
            LogPrint(BCLog::REDDID, "Unknown namespace %s for user ID auction\n", msg.namespaceId);
            return false;
        }

        // Validate user ID format based on namespace rules
        if (msg.name.size() < namespaceInfo.minLength || msg.name.size() > namespaceInfo.maxLength) {
            LogPrint(BCLog::REDDID, "Invalid name length for %s.%s: %zu (should be %d-%d)\n",
                    msg.name, msg.namespaceId, msg.name.size(),
                    namespaceInfo.minLength, namespaceInfo.maxLength);
            return false;
        }

        // Check character validation based on namespace rules
        bool nameValid = true;
        for (const char& c : msg.name) {
            bool charValid = (c >= 'a' && c <= 'z');  // Letters always allowed

            // Check if numbers are allowed
            if (!charValid && namespaceInfo.allowNumbers && (c >= '0' && c <= '9')) {
                charValid = true;
            }

            // Check if hyphens are allowed
            if (!charValid && namespaceInfo.allowHyphens && c == '-') {
                charValid = true;
            }

            // Check if underscores are allowed
            if (!charValid && namespaceInfo.allowUnderscores && c == '_') {
                charValid = true;
            }

            if (!charValid) {
                nameValid = false;
                break;
            }
        }

        if (!nameValid) {
            LogPrint(BCLog::REDDID, "Invalid characters in name: %s.%s\n", msg.name, msg.namespaceId);
            return false;
        }

        // Check if name already exists and is not expired
        UserIDInfo existingUserID;
        if (reddIDManager && reddIDManager->GetUserIDInfo(msg.name, msg.namespaceId, existingUserID)) {
            int64_t currentTime = GetTime();
            if (existingUserID.expirationTime > currentTime) {
                LogPrint(BCLog::REDDID, "User ID %s.%s already exists and has not expired\n",
                        msg.name, msg.namespaceId);
                return false;
            }
        }

        // Check if we already know about this auction
        AuctionInfo existingAuction;
        if (auctionManager->GetAuctionInfo(msg.auctionId, existingAuction)) {
            LogPrint(BCLog::REDDID, "Already know about auction %s\n", msg.auctionId.ToString());
            return true;
        }

        // Validate auction duration against namespace rules
        int durationDays = (msg.endTime - msg.startTime) / (24 * 60 * 60);
        if (durationDays < namespaceInfo.minAuctionDuration || durationDays > namespaceInfo.maxAuctionDuration) {
            LogPrint(BCLog::REDDID, "Invalid auction duration for %s.%s: %d days (should be %d-%d)\n",
                    msg.name, msg.namespaceId, durationDays,
                    namespaceInfo.minAuctionDuration, namespaceInfo.maxAuctionDuration);
            return false;
        }

        // Check reserve price against namespace rules
        CAmount minPrice = 0;
        if (reddIDManager) {
            minPrice = reddIDManager->CalculateMinPriceForUserID(msg.name, msg.namespaceId);
        } else {
            // Fallback to default minimum
            minPrice = REDDID_MIN_FEE;
        }

        if (msg.reservePrice < minPrice) {
            LogPrint(BCLog::REDDID, "Reserve price too low for %s.%s: %s (minimum: %s)\n",
                    msg.name, msg.namespaceId, FormatMoney(msg.reservePrice), FormatMoney(minPrice));
            return false;
        }

        // Create the auction object
        AuctionInfo auction;
        auction.auctionId = msg.auctionId;
        auction.name = msg.name;
        auction.namespaceId = msg.namespaceId;
        auction.creator = msg.creator;
        auction.startTime = msg.startTime;
        auction.endTime = msg.endTime;
        auction.reservePrice = msg.reservePrice;
        auction.currentBid = 0;
        auction.type = msg.type;
        auction.state = AUCTION_PENDING;
        auction.depositAmount = auction.reservePrice * 0.1;  // Default 10% deposit

        // Save the auction
        uint256 auctionId = msg.auctionId;
        if (!auctionManager->CreateAuction(auction, auctionId)) {
            LogPrint(BCLog::REDDID, "Failed to create user ID auction from announcement\n");
            return false;
        }

        LogPrint(BCLog::REDDID, "Successfully created user ID auction %s for %s.%s from P2P announcement\n",
                auctionId.ToString(), msg.name, msg.namespaceId);

        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the announcement to other nodes
        RelayMessage(MSG_USERID_AUCTION_ANNOUNCE, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing user ID auction announcement: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessUserIDAuctionBid(CNode* pfrom, const CAuctionBid& msg) {
    LogPrint(BCLog::REDDID, "Received user ID auction bid for auction %s\n", msg.auctionId.ToString());

    // Check if we know about this auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(msg.auctionId, auction)) {
        LogPrint(BCLog::REDDID, "Unknown auction %s\n", msg.auctionId.ToString());
        return false;
    }

    // Check if this is a user ID auction
    if (auction.name.empty()) {
        LogPrint(BCLog::REDDID, "Auction %s is not a user ID auction\n", msg.auctionId.ToString());
        return false;
    }

    // Check if the auction is active
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        LogPrint(BCLog::REDDID, "Auction %s is not active\n", msg.auctionId.ToString());
        return false;
    }

    // Check if bid amount is greater than current bid
    if (auction.currentBid > 0) {
        // Calculate minimum increment (based on namespace settings)
        NamespaceInfo namespaceInfo;
        double minIncrement = 0.05;  // Default 5%

        if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
            minIncrement = namespaceInfo.minBidIncrement / 100.0;
        }

        CAmount minBid = auction.currentBid * (1.0 + minIncrement);
        if (msg.bidAmount < minBid) {
            LogPrint(BCLog::REDDID, "Bid amount %s is less than minimum bid %s\n",
                    FormatMoney(msg.bidAmount), FormatMoney(minBid));
            return false;
        }
    } else if (msg.bidAmount < auction.reservePrice) {
        // For first bid, check against reserve price
        LogPrint(BCLog::REDDID, "Bid amount %s is less than reserve price %s\n",
                FormatMoney(msg.bidAmount), FormatMoney(auction.reservePrice));
        return false;
    }

    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the bid to other nodes
    RelayMessage(MSG_USERID_AUCTION_BID, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessUserIDAuctionFinalize(CNode* pfrom, const CAuctionFinalize& msg) {
    LogPrint(BCLog::REDDID, "Received user ID auction finalization for auction %s\n", msg.auctionId.ToString());

    // Check if we know about this auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(msg.auctionId, auction)) {
        LogPrint(BCLog::REDDID, "Unknown auction %s\n", msg.auctionId.ToString());
        return false;
    }

    // Check if this is a user ID auction
    if (auction.name.empty()) {
        LogPrint(BCLog::REDDID, "Auction %s is not a user ID auction\n", msg.auctionId.ToString());
        return false;
    }

    // Check if the auction is in a state that can be finalized
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
        LogPrint(BCLog::REDDID, "Auction %s cannot be finalized in state %d\n",
                msg.auctionId.ToString(), auction.state);
        return false;
    }

    // Check if the auction has ended
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime) {
        LogPrint(BCLog::REDDID, "Auction %s has not ended yet\n", msg.auctionId.ToString());
        return false;
    }

    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the finalization to other nodes
    RelayMessage(MSG_USERID_AUCTION_FINALIZE, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessUserIDAuctionCancel(CNode* pfrom, const uint256& auctionId) {
    LogPrint(BCLog::REDDID, "Received user ID auction cancellation for auction %s\n", auctionId.ToString());

    // Check if we know about this auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        LogPrint(BCLog::REDDID, "Unknown auction %s\n", auctionId.ToString());
        return false;
    }

    // Check if this is a user ID auction
    if (auction.name.empty()) {
        LogPrint(BCLog::REDDID, "Auction %s is not a user ID auction\n", auctionId.ToString());
        return false;
    }

    // Check if the auction is in a state that can be canceled
    if (auction.state != AUCTION_PENDING && auction.state != AUCTION_ACTIVE) {
        LogPrint(BCLog::REDDID, "Auction %s cannot be canceled in state %d\n",
                auctionId.ToString(), auction.state);
        return false;
    }

    // If the auction has bids, it can only be canceled if it's pending
    if (auction.state == AUCTION_ACTIVE && auction.currentBid > 0) {
        LogPrint(BCLog::REDDID, "Active auction %s with bids cannot be canceled\n", auctionId.ToString());
        return false;
    }

    // Create a data stream to serialize the message for relay
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << auctionId;

    // Forward the cancellation to other nodes
    RelayMessage(MSG_USERID_AUCTION_CANCEL, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessReddIDProfileUpdate(CNode* pfrom, const CReddIDProfileUpdate& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received profile update for %s\n", msg.reddId);

        // Validate ReddID format
        if (msg.reddId.empty() || msg.reddId.length() < MIN_REDDID_LENGTH || msg.reddId.length() > MAX_REDDID_LENGTH) {
            LogPrint(BCLog::REDDID, "Invalid ReddID length: %s\n", msg.reddId);
            return false;
        }

        // Check allowed characters
        for (const char& c : msg.reddId) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                LogPrint(BCLog::REDDID, "Invalid character in ReddID: %s\n", msg.reddId);
                return false;
            }
        }

        // Cannot begin or end with underscore
        if (msg.reddId[0] == '_' || msg.reddId[msg.reddId.size() - 1] == '_') {
            LogPrint(BCLog::REDDID, "ReddID cannot begin or end with underscore: %s\n", msg.reddId);
            return false;
        }

        // Check display name length
        if (msg.profile.displayName.size() > 64) {
            LogPrint(BCLog::REDDID, "Display name too long: %s\n", msg.reddId);
            return false;
        }

        // Check bio length
        if (msg.profile.bio.size() > 256) {
            LogPrint(BCLog::REDDID, "Bio too long: %s\n", msg.reddId);
            return false;
        }

        // Check if we already have this profile
        ReddIDProfile existingProfile;
        bool haveProfile = profileManager->GetProfile(msg.reddId, existingProfile);

        if (haveProfile) {
            // We already have this profile, check if it's more recent
            if (msg.profile.lastUpdated <= existingProfile.lastUpdated) {
                LogPrint(BCLog::REDDID, "Already have more recent profile for %s\n", msg.reddId);
                return true;
            }
        }

        // Update the profile using a public method
        bool success = profileManager->UpdateProfile(msg.profile);

        if (success) {
            // Create a data stream to serialize the message
            CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
            data << msg;

            // Forward the update to other nodes
            RelayMessage(MSG_REDDID_PROFILE_UPDATE, data, {pfrom->GetId()});

            LogPrint(BCLog::REDDID, "Successfully processed profile update for %s\n", msg.reddId);
        } else {
            LogPrint(BCLog::REDDID, "Failed to update profile for %s\n", msg.reddId);
        }

        return success;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing profile update: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessReddIDProfileRequest(CNode* pfrom, const CReddIDProfileRequest& msg) {
    LogPrintf("Received profile request for %s\n", msg.reddId);

    // Check if we have this profile
    ReddIDProfile profile;
    if (!profileManager->GetProfile(msg.reddId, profile)) {
        LogPrintf("Don't have profile for %s\n", msg.reddId);
        return false;
    }

    // Send the profile
    CReddIDProfileUpdate update;
    update.reddId = msg.reddId;
    update.profile = profile;

    std::vector<unsigned char> profileData(msg.reddId.begin(), msg.reddId.end());
    update.profileHash = Hash(profileData);  // Use the simpler Hash function for vectors

    update.timestamp = GetTime();

    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << update;

    // Forward the announcement to other nodes
    RelayMessage(MSG_REDDID_PROFILE_RESPONSE, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessReddIDProfileResponse(CNode* pfrom, const CReddIDProfileUpdate& msg) {
    LogPrint(BCLog::REDDID, "Received profile response for %s\n", msg.reddId);

    // Check if we have a pending request for this profile
    // In a full implementation, we would track pending requests and verify this response matches one

    // Validate the profile contents
    bool isValid = true;

    // Check ReddID format
    if (msg.reddId.empty() || msg.reddId.length() < MIN_REDDID_LENGTH ||
        msg.reddId.length() > MAX_REDDID_LENGTH) {
        LogPrint(BCLog::REDDID, "Invalid ReddID format: %s\n", msg.reddId);
        isValid = false;
    }

    // Check profile fields
    if (msg.profile.displayName.length() > 64) {
        LogPrint(BCLog::REDDID, "Display name too long for ReddID %s\n", msg.reddId);
        isValid = false;
    }

    if (msg.profile.bio.length() > 256) {
        LogPrint(BCLog::REDDID, "Bio too long for ReddID %s\n", msg.reddId);
        isValid = false;
    }

    if (!isValid) {
        LogPrint(BCLog::REDDID, "Invalid profile data received for %s\n", msg.reddId);
        return false;
    }

    // Check if we already have this profile
    ReddIDProfile existingProfile;
    bool haveProfile = profileManager->GetProfile(msg.reddId, existingProfile);

    if (haveProfile && msg.profile.lastUpdated <= existingProfile.lastUpdated) {
        LogPrint(BCLog::REDDID, "Already have more recent profile for %s\n", msg.reddId);
        return true;
    }

    // Update our profile database
    bool success = profileManager->UpdateProfile(msg.profile);

    if (success) {
        LogPrint(BCLog::REDDID, "Updated profile for %s from peer response\n", msg.reddId);
    } else {
        LogPrint(BCLog::REDDID, "Failed to update profile for %s\n", msg.reddId);
    }

    return success;
}

bool ReddIDP2PManager::ProcessReddIDConnection(CNode* pfrom, const CReddIDConnection& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received connection from %s to %s\n",
                 msg.connection.fromReddId, msg.connection.toReddId);

        // Validate the connection
        if (msg.connection.fromReddId == msg.connection.toReddId) {
            LogPrint(BCLog::REDDID, "Self-connection not allowed from %s\n", msg.connection.fromReddId);
            return false;
        }

        // Check valid connection type
        if (msg.connection.connectionType < CONNECTION_FOLLOW ||
            msg.connection.connectionType > CONNECTION_BLOCK) {
            LogPrint(BCLog::REDDID, "Invalid connection type from %s to %s\n",
                    msg.connection.fromReddId, msg.connection.toReddId);
            return false;
        }

        // Check valid visibility
        if (msg.connection.visibility < 0 || msg.connection.visibility > 2) {
            LogPrint(BCLog::REDDID, "Invalid visibility from %s to %s\n",
                    msg.connection.fromReddId, msg.connection.toReddId);
            return false;
        }

        // Check if the profiles exist
        if (!profileManager->ProfileExists(msg.connection.fromReddId) ||
            !profileManager->ProfileExists(msg.connection.toReddId)) {
            LogPrint(BCLog::REDDID, "One or both profiles don't exist\n");
            return false;
        }

        // Check if we already know about this connection
        ReddIDConnection existingConnection;
        if (profileManager->GetConnection(msg.connection.fromReddId, msg.connection.toReddId, existingConnection)) {
            LogPrint(BCLog::REDDID, "Connection from %s to %s already exists\n",
                    msg.connection.fromReddId, msg.connection.toReddId);

            // Check if the received connection is more recent
            if (msg.connection.lastInteraction <= existingConnection.lastInteraction) {
                LogPrint(BCLog::REDDID, "Already have more recent connection data\n");
                return true;
            }
        }

        // Save the connection locally
        bool success = profileManager->CreateConnection(msg.connection);

        if (success) {
            LogPrint(BCLog::REDDID, "Successfully saved connection from %s to %s\n",
                    msg.connection.fromReddId, msg.connection.toReddId);

            // Create a data stream to serialize the message
            CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
            data << msg;

            // Forward the connection to other nodes
            RelayMessage(MSG_REDDID_CONNECTION, data, {pfrom->GetId()});
        } else {
            LogPrint(BCLog::REDDID, "Failed to save connection from %s to %s\n",
                    msg.connection.fromReddId, msg.connection.toReddId);
        }

        return success;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing ReddID connection: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessReddIDReputationUpdate(CNode* pfrom, const CReddIDReputationUpdate& msg) {
    try {
        LogPrint(BCLog::REDDID, "Received reputation update for %s\n", msg.reputation.reddId);

        // Check if the profile exists
        if (!profileManager->ProfileExists(msg.reputation.reddId)) {
            LogPrint(BCLog::REDDID, "Profile %s doesn't exist\n", msg.reputation.reddId);
            return false;
        }

        // Validate reputation scores - all scores should be between 0 and 100
        if (msg.reputation.overallScore < 0 || msg.reputation.overallScore > 100 ||
            msg.reputation.longevityScore < 0 || msg.reputation.longevityScore > 100 ||
            msg.reputation.transactionScore < 0 || msg.reputation.transactionScore > 100 ||
            msg.reputation.engagementScore < 0 || msg.reputation.engagementScore > 100 ||
            msg.reputation.verificationScore < 0 || msg.reputation.verificationScore > 100 ||
            msg.reputation.auctionScore < 0 || msg.reputation.auctionScore > 100) {
            LogPrint(BCLog::REDDID, "Invalid reputation score values for %s: overall=%.1f, longevity=%.1f, "
                    "transaction=%.1f, engagement=%.1f, verification=%.1f, auction=%.1f\n",
                    msg.reputation.reddId,
                    msg.reputation.overallScore,
                    msg.reputation.longevityScore,
                    msg.reputation.transactionScore,
                    msg.reputation.engagementScore,
                    msg.reputation.verificationScore,
                    msg.reputation.auctionScore);
            return false;
        }

        // Check if we already have a more recent reputation score
        ReddIDReputation existingRep;
        if (profileManager->GetReputation(msg.reputation.reddId, existingRep)) {
            if (msg.reputation.lastCalculated <= existingRep.lastCalculated) {
                LogPrint(BCLog::REDDID, "Already have more recent reputation for %s\n", msg.reputation.reddId);
                return true;
            }
        }

        // Update the reputation in our database
        bool success = profileManager->UpdateReputation(msg.reputation);

        if (!success) {
            LogPrint(BCLog::REDDID, "Failed to update reputation for %s\n", msg.reputation.reddId);
            return false;
        }

        LogPrint(BCLog::REDDID, "Successfully updated reputation for %s from P2P announcement\n",
                 msg.reputation.reddId);

        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the update to other nodes
        RelayMessage(MSG_REDDID_REPUTATION_UPDATE, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error processing reputation update: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::AnnounceNamespaceAuction(const AuctionInfo& auction) {
    if (!m_running) {
        return false;
    }

    CNamespaceAuctionAnnounce announce;
    announce.auctionId = auction.auctionId;
    announce.namespaceId = auction.namespaceId;
    announce.creator = auction.creator;  // Add creator field
    announce.startTime = auction.startTime;
    announce.endTime = auction.endTime;
    announce.reservePrice = auction.reservePrice;
    announce.type = auction.type;
    announce.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << announce;

    // Relay to all subscribed nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_ANNOUNCE, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceNamespaceBid(const BidInfo& bid) {
    if (!m_running) {
        return false;
    }

    CAuctionBid bidMsg;
    bidMsg.auctionId = bid.auctionId;
    bidMsg.bidId = bid.bidId;
    bidMsg.bidder = bid.bidder;    // Include bidder information
    bidMsg.bidAmount = bid.bidAmount;
    bidMsg.depositAmount = bid.depositAmount;
    bidMsg.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << bidMsg;

    // Relay to all subscribed nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_BID, data, {});

    return true;
}

// bool ReddIDP2PManager::AnnounceNamespaceFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice) {
//    if (!m_running) {
//        return false;
//    }
//
//    CAuctionFinalize finalize;
//    finalize.auctionId = auctionId;
//    finalize.winningBidId = winningBidId;
//    finalize.finalPrice = finalPrice;
//    finalize.timestamp = GetTime();
//
//    // Serialize to data stream
//    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
//    data << finalize;
//
//    // Relay to all subscribed nodes
//    RelayMessage(MSG_NAMESPACE_AUCTION_FINALIZE, data, {});
//
//    return true;
// }

bool ReddIDP2PManager::AnnounceNamespaceCancel(const uint256& auctionId) {
    if (!m_running) {
        return false;
    }

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << auctionId;

    // Relay to all subscribed nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_CANCEL, data, {});

    return true;
}

bool ReddIDP2PManager::SendNamespaceConfig(CNode* pfrom, const std::string& namespaceId) {
    if (!m_running || !node || !node->connman) {
        return false;
    }

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return false;
    }

    // Get pricing tiers
    std::vector<PricingTier> tiers = namespaceManager->GetPricingTiers(namespaceId);

    // Create config message
    CNamespaceConfig config;
    config.config = namespaceInfo;
    config.pricingTiers = tiers;
    config.timestamp = GetTime();

    // Create message and send
    CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << config;

    node->connman->PushMessage(pfrom, msgMaker.Make(MSG_NAMESPACE_CONFIG_RESPONSE, data));

    return true;
}

bool ReddIDP2PManager::RequestNamespaceConfig(const std::string& namespaceId) {
    if (!m_running || !node || !node->connman) {
        return false;
    }

    CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    // Request from first available subscribed node
    bool sentRequest = false;
    node->connman->ForEachNode([&](CNode* pnode) {
        if (sentRequest) {
            return;  // Already sent a request
        }

        if (IsNodeSubscribed(pnode->GetId())) {
            CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
            data << namespaceId;
            node->connman->PushMessage(pnode, msgMaker.Make(MSG_NAMESPACE_CONFIG_REQUEST, data));
            sentRequest = true;
        }
    });

    return sentRequest;
}

bool ReddIDP2PManager::RequestAllNamespaceConfigs(CNode* targetNode) {
    if (!m_running || !node || !node->connman || !namespaceManager) {
        LogPrint(BCLog::REDDID, "Cannot request all namespaces: manager or connman not available\n");
        return false;
    }

    // Get all known namespaces
    std::vector<NamespaceInfo> allNamespaces = namespaceManager->GetNamespaces();

    if (allNamespaces.empty()) {
        LogPrint(BCLog::REDDID, "No namespaces to request configs for\n");
        return true;  // Not an error, just nothing to do
    }

    CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    bool sentAnyRequest = false;

    for (const auto& ns : allNamespaces) {
        if (targetNode) {
            // Send to specific node
            if (IsNodeSubscribed(targetNode->GetId())) {
                CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
                data << ns.id;
                node->connman->PushMessage(targetNode, msgMaker.Make(MSG_NAMESPACE_CONFIG_REQUEST, data));
                sentAnyRequest = true;
                LogPrint(BCLog::REDDID, "Requested namespace config for %s from node %d\n",
                        ns.id, targetNode->GetId());
            }
        } else {
            // Send to first available subscribed node
            node->connman->ForEachNode([&](CNode* pnode) {
                if (IsNodeSubscribed(pnode->GetId())) {
                    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
                    data << ns.id;
                    node->connman->PushMessage(pnode, msgMaker.Make(MSG_NAMESPACE_CONFIG_REQUEST, data));
                    sentAnyRequest = true;
                    LogPrint(BCLog::REDDID, "Requested namespace config for %s from node %d\n",
                            ns.id, pnode->GetId());
                    return false;  // Stop after first node
                }
                return true;  // Continue to next node
            });
        }
    }

    LogPrint(BCLog::REDDID, "Requested configs for %d namespaces\n", allNamespaces.size());
    return sentAnyRequest;
}

bool ReddIDP2PManager::AnnounceUserIDAuction(const AuctionInfo& auction) {
    if (!m_running) {
        return false;
    }

    CUserIDAuctionAnnounce announce;
    announce.auctionId = auction.auctionId;
    announce.name = auction.name;
    announce.namespaceId = auction.namespaceId;
    announce.creator = auction.creator;  // Add creator field
    announce.startTime = auction.startTime;
    announce.endTime = auction.endTime;
    announce.reservePrice = auction.reservePrice;
    announce.type = auction.type;
    announce.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << announce;

    // Relay to all subscribed nodes
    RelayMessage(MSG_USERID_AUCTION_ANNOUNCE, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceUserIDBid(const BidInfo& bid) {
    if (!m_running) {
        return false;
    }

    CAuctionBid bidMsg;
    bidMsg.auctionId = bid.auctionId;
    bidMsg.bidId = bid.bidId;
    bidMsg.bidder = bid.bidder;    // Include bidder information
    bidMsg.bidAmount = bid.bidAmount;
    bidMsg.depositAmount = bid.depositAmount;
    bidMsg.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << bidMsg;

    // Relay to all subscribed nodes
    RelayMessage(MSG_USERID_AUCTION_BID, data, {});

    return true;
}

// bool ReddIDP2PManager::AnnounceUserIDFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice) {
//    try {
//        if (!m_running) {
//            LogPrint(BCLog::REDDID, "Cannot announce UserID auction finalization: manager not running\n");
//            return false;
//        }
//
//        LogPrint(BCLog::REDDID, "Announcing UserID auction finalization for auction %s\n", auctionId.ToString());
//
//        // Default creator to empty key
//        CKeyID creator;
//
//        // First update the local auction state
//        if (auctionManager) {
//            // Get the current auction info
//            AuctionInfo auction;
//            if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
//                LogPrint(BCLog::REDDID, "Failed to get auction info for %s\n", auctionId.ToString());
//                return false;
//            }
//
//            // Store the creator for the message
//            creator = auction.creator;
//
//            // Verify it's a user ID auction
//            if (auction.name.empty()) {
//                LogPrint(BCLog::REDDID, "Auction %s is not a user ID auction\n", auctionId.ToString());
//                return false;
//            }
//
//            // Check if the auction is in a state that can be finalized
//            if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
//                LogPrint(BCLog::REDDID, "Auction %s cannot be finalized in state %d\n",
//                        auctionId.ToString(), auction.state);
//                return false;
//            }
//
//            // Validate the winning bid exists
//            BidInfo winningBid;
//            if (!auctionManager->GetBidInfo(winningBidId, winningBid)) {
//                LogPrint(BCLog::REDDID, "Winning bid %s not found for auction %s\n",
//                       winningBidId.ToString(), auctionId.ToString());
//                return false;
//            }
//
//            // Update the auction state
//            auction.state = AUCTION_FINALIZED;
//            auction.winningBidId = winningBidId;
//            auction.finalPrice = finalPrice;
//
//            // Save the updated auction
//            if (!auctionManager->UpdateAuction(auction)) {
//                LogPrint(BCLog::REDDID, "Failed to update auction %s\n", auctionId.ToString());
//                return false;
//            }
//
//            LogPrint(BCLog::REDDID, "Successfully finalized auction %s with winning bid %s for %s\n",
//                    auctionId.ToString(), winningBidId.ToString(), FormatMoney(finalPrice));
//        } else {
//            LogPrint(BCLog::REDDID, "Warning: Auction manager not available, can't update local state\n");
//            // Continue anyway to relay the message
//        }
//
//        // Create message for relay
//        CAuctionFinalize finalize;
//        finalize.auctionId = auctionId;
//        finalize.winningBidId = winningBidId;
//        finalize.finalPrice = finalPrice;
//        finalize.creator = creator;  // Set the creator field
//        finalize.timestamp = GetTime();
//
//        // Serialize to data stream
//        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
//        data << finalize;
//
//        // Relay to all subscribed nodes
//        RelayMessage(MSG_USERID_AUCTION_FINALIZE, data, {});
//
//        return true;
//    } catch (const std::exception& e) {
//        LogPrint(BCLog::REDDID, "Error announcing user ID auction finalization: %s\n", e.what());
//        return false;
//    }
// }

bool ReddIDP2PManager::AnnounceUserIDCancel(const uint256& auctionId) {
    if (!m_running) {
        return false;
    }

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << auctionId;

    // Relay to all subscribed nodes
    RelayMessage(MSG_USERID_AUCTION_CANCEL, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceReddIDAuction(const AuctionInfo& auction) {
    if (!m_running) {
        return false;
    }

    // Similar to user ID auction announce
    CUserIDAuctionAnnounce announce;
    announce.auctionId = auction.auctionId;
    announce.name = auction.name;
    announce.namespaceId = auction.namespaceId;
    announce.startTime = auction.startTime;
    announce.endTime = auction.endTime;
    announce.reservePrice = auction.reservePrice;
    announce.type = auction.type;
    announce.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << announce;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_AUCTION_ANNOUNCE, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceReddIDBid(const BidInfo& bid) {
    if (!m_running) {
        return false;
    }

    // Similar to user ID bid
    CAuctionBid bidMsg;
    bidMsg.auctionId = bid.auctionId;
    bidMsg.bidId = bid.bidId;
    bidMsg.bidAmount = bid.bidAmount;
    bidMsg.depositAmount = bid.depositAmount;
    bidMsg.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << bidMsg;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_AUCTION_BID, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceReddIDFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice) {
    if (!m_running) {
        return false;
    }

    // Similar to user ID finalize
    CAuctionFinalize finalize;
    finalize.auctionId = auctionId;
    finalize.winningBidId = winningBidId;
    finalize.finalPrice = finalPrice;
    finalize.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << finalize;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_AUCTION_FINALIZE, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceProfileUpdate(const std::string& reddId, const ReddIDProfile& profile) {
    if (!m_running) {
        return false;
    }

    CReddIDProfileUpdate update;
    update.reddId = reddId;
    update.profile = profile;

    std::vector<unsigned char> profileData(profile.reddId.begin(), profile.reddId.end());
    update.profileHash = Hash(profileData);  // Use the simpler Hash function for vectors
    update.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << update;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_PROFILE_UPDATE, data, {});

    return true;
}

bool ReddIDP2PManager::RequestProfile(const std::string& reddId) {
    if (!m_running || !node || !node->connman) {
        return false;
    }

    CReddIDProfileRequest request;
    request.reddId = reddId;
    request.timestamp = GetTime();

    CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << request;

    // Request from first available subscribed node
    bool sentRequest = false;
    node->connman->ForEachNode([&](CNode* pnode) {
        if (sentRequest) {
            return;  // Already sent a request
        }

        if (IsNodeSubscribed(pnode->GetId())) {
            node->connman->PushMessage(pnode, msgMaker.Make(MSG_REDDID_PROFILE_REQUEST, data));
            sentRequest = true;
        }
    });

    return sentRequest;
}

bool ReddIDP2PManager::AnnounceConnection(const ReddIDConnection& connection) {
    if (!m_running) {
        return false;
    }

    CReddIDConnection connMsg;
    connMsg.connection = connection;
    connMsg.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << connMsg;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_CONNECTION, data, {});

    return true;
}

bool ReddIDP2PManager::AnnounceReputationUpdate(const ReddIDReputation& reputation) {
    if (!m_running) {
        return false;
    }

    CReddIDReputationUpdate update;
    update.reputation = reputation;
    update.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << update;

    // Relay to all subscribed nodes
    RelayMessage(MSG_REDDID_REPUTATION_UPDATE, data, {});

    return true;
}

bool ReddIDP2PManager::SubscribeNode(NodeId nodeId) {
    // Now update our subscription list
    {
        LOCK(cs_subscriptions);

        // Check if already subscribed
        if (subscribedNodes.find(nodeId) != subscribedNodes.end()) {
            LogPrint(BCLog::REDDID, "Node %d already subscribed to ReddID updates\n", nodeId);
            return true;  // Already subscribed, but that's okay
        }

        subscribedNodes.insert(nodeId);
    }

    LogPrint(BCLog::REDDID, "Successfully subscribed node %d to ReddID updates\n", nodeId);
    return true;
}

bool ReddIDP2PManager::IsNodeSubscribed(NodeId nodeId) const {
    LOCK(cs_subscriptions);
    return subscribedNodes.find(nodeId) != subscribedNodes.end();
}

std::vector<NodeId> ReddIDP2PManager::GetSubscribedNodes() const {
    LOCK(cs_subscriptions);
    return std::vector<NodeId>(subscribedNodes.begin(), subscribedNodes.end());
}

bool ReddIDP2PManager::UnsubscribeNode(NodeId nodeId) {
    LOCK(cs_subscriptions);
    subscribedNodes.erase(nodeId);
    return true;
}

void ReddIDP2PManager::OnNodeConnected(CNode* pnode) {
    if (!pnode) return;

    if (pnode->GetLocalServices() & NODE_REDDID) {
        SubscribeNode(pnode->GetId());
        std::string serviceStr = Join(serviceFlagsToStr(pnode->GetLocalServices()), "|");
        LogPrint(BCLog::REDDID, "Connected to ReddID-capable node %d (services %s)\n",
                 pnode->GetId(), serviceStr);

        RequestAllNamespaceConfigs(pnode);  // Request all namespaces
    } else {
        std::string serviceStr = Join(serviceFlagsToStr(pnode->GetLocalServices()), "|");
        LogPrint(BCLog::REDDID, "Connected to non-ReddID node %d (services %s)\n",
                 pnode->GetId(), serviceStr);
    }
}

void ReddIDP2PManager::OnNodeDisconnected(NodeId nodeId) {
    UnsubscribeNode(nodeId);

    // Clean up rate limiting data
    LOCK(cs_message_rates);
    m_messageRateLimits.erase(nodeId);
}

std::vector<NodeId> ReddIDP2PManager::GetReddIDNodes() const {
    std::vector<NodeId> result;

    if (!node || !node->connman) {
        return result;
    }

    node->connman->ForEachNode([&](CNode* pnode) {
        if (pnode->GetLocalServices() & NODE_REDDID) {
            result.push_back(pnode->GetId());
        }
    });

    return result;
}

bool ReddIDP2PManager::HasReddIDPeers() const {
    if (!node || !node->connman) {
        return false;
    }

    bool hasReddIDPeer = false;

    node->connman->ForEachNode([&](CNode* pnode) {
        if (pnode->GetLocalServices() & NODE_REDDID) {
            hasReddIDPeer = true;
            return false;  // Stop iteration
        }
        return true;  // Continue iteration
    });

    return hasReddIDPeer;
}

bool ReddIDP2PManager::IsReddIDEnabled() const {
    return (connman->GetLocalServices() & NODE_REDDID) != 0;
}

void ReddIDP2PManager::RelayMessage(const std::string& command, const CDataStream& data,
                                   const std::vector<NodeId>& exceptNodes) {
    // Check if the node context and connman are available
    if (!node || !node->connman) {
        LogPrint(BCLog::REDDID, "Cannot relay message: node or connman not available\n");
        return;
    }

    // Only relay if we're advertising ReddID support
    if (!IsReddIDEnabled()) {
        LogPrint(BCLog::REDDID, "Not relaying ReddID messages - service not enabled\n");
        return;
    }

    CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    // Manually apply exception filter and subscription filter
    std::vector<CNode*> nodes_to_send;

    try {
        node->connman->ForEachNode([&](CNode* pnode) {
            if (!pnode) {
                LogPrint(BCLog::REDDID, "Null node encountered during relay\n");
                return;
            }

            NodeId nodeId = pnode->GetId();

            // Skip if node is in except list
            if (std::find(exceptNodes.begin(), exceptNodes.end(), nodeId) != exceptNodes.end()) {
                return;
            }

            // Only send to subscribed nodes for efficiency
            if (IsNodeSubscribed(nodeId)) {
                nodes_to_send.push_back(pnode);
            }
        });

        // Send to all filtered nodes
        int sent_count = 0;
        for (CNode* pnode : nodes_to_send) {
            if (!pnode) {
                LogPrint(BCLog::REDDID, "Null node encountered during message push\n");
                continue;
            }

            try {
                node->connman->PushMessage(pnode, msgMaker.Make(command, data));
                sent_count++;
            } catch (const std::exception& e) {
                LogPrint(BCLog::REDDID, "Error sending message to node %d: %s\n",
                         pnode->GetId(), e.what());
            }
        }

        LogPrint(BCLog::REDDID, "Relayed %s message to %d nodes\n", command, sent_count);
    }
    catch (const std::exception& e) {
        LogPrint(BCLog::REDDID, "Error during message relay: %s\n", e.what());
    }
}

bool ReddIDP2PManager::CheckMessageRate(CNode* pfrom, const std::string& strCommand) {
    LOCK(cs_message_rates);

    if (!pfrom) {
        LogPrint(BCLog::REDDID, "CheckMessageRate: null pfrom\n");
        return false;
    }

    const NodeId nodeId = pfrom->GetId();
    const int64_t now = GetTime();

    // Get or initialize rate limiting data for this node
    auto& nodeRateLimits = m_messageRateLimits[nodeId];
    auto& messageData = nodeRateLimits[strCommand];

    // Get last time and count
    int64_t& lastTime = messageData.first;
    int& messageCount = messageData.second;

    // Define rate limits for different message types
    int maxMessagesPerMinute = DEFAULT_MAX_MESSAGES_PER_MINUTE;  // Default

    if (strCommand == MSG_NAMESPACE_AUCTION_ANNOUNCE ||
        strCommand == MSG_USERID_AUCTION_ANNOUNCE ||
        strCommand == MSG_REDDID_AUCTION_ANNOUNCE) {
        maxMessagesPerMinute = MAX_AUCTION_ANNOUNCES_PER_MINUTE;
    } else if (strCommand == MSG_NAMESPACE_AUCTION_BID ||
             strCommand == MSG_USERID_AUCTION_BID ||
             strCommand == MSG_REDDID_AUCTION_BID) {
        maxMessagesPerMinute = MAX_AUCTION_BIDS_PER_MINUTE;
    } else if (strCommand == MSG_REDDID_PROFILE_UPDATE) {
        maxMessagesPerMinute = MAX_PROFILE_UPDATES_PER_MINUTE;
    } else if (strCommand == MSG_REDDID_CONNECTION) {
        maxMessagesPerMinute = MAX_CONNECTIONS_PER_MINUTE;
    }

    // Reset counter if rate limit window has passed
    if (lastTime < now - RATE_LIMIT_WINDOW_SECONDS) {
        messageCount = 1;
        lastTime = now;
        return true;
    }

    // Increment counter and check limit
    messageCount++;
    if (messageCount > maxMessagesPerMinute) {
        LogPrint(BCLog::REDDID, "Rate limit exceeded for %s messages from node %d: %d in last minute\n",
                strCommand, nodeId, messageCount);
        return false;
    }

    return true;
}

void ReddIDP2PManager::CleanupRateLimitData() {
    const int64_t now = GetTime();
    const int64_t cutoff = now - (RATE_LIMIT_WINDOW_SECONDS * 5);  // 5 windows old

    for (auto it = m_messageRateLimits.begin(); it != m_messageRateLimits.end(); ) {
        bool anyRecent = false;
        for (const auto& msg : it->second) {
            if (msg.second.first > cutoff) {
                anyRecent = true;
                break;
            }
        }

        if (!anyRecent) {
            it = m_messageRateLimits.erase(it);
        } else {
            ++it;
        }
    }
}
