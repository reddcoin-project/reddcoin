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
#include <validation.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

ReddIDP2PManager::ReddIDP2PManager(NodeContext& nodeIn)
    : running(false), reddIDManager(nullptr), node(&nodeIn), connman(nullptr) {
}

ReddIDP2PManager::~ReddIDP2PManager() {
}

bool ReddIDP2PManager::Init() {
    LogPrintf("Initializing ReddID P2P manager\n");
    return true;
}

bool ReddIDP2PManager::Start() {
    LogPrintf("Starting ReddID P2P manager\n");
    
    if (running) {
        return true;
    }
    
    running = true;
    return true;
}

void ReddIDP2PManager::Interrupt() {
    LogPrintf("Interrupting ReddID P2P manager\n");
    running = false;
}

bool ReddIDP2PManager::Stop() {
    LogPrintf("Stopping ReddID P2P manager\n");
    
    if (!running) {
        return true;
    }
    
    SetManagerActive(false);
    return true;
}

void ReddIDP2PManager::SetManagerActive(bool active) {
    LogPrintf("ReddIDP2PManager: %s: %s\n", __func__, active);

    if (running == active) {
	return;
    }

    running = active;
}

void ReddIDP2PManager::ConnectReddIDManager(ReddIDManager& manager) {
    reddIDManager = &manager;
    namespaceManager = manager.GetNamespaceManager();
    auctionManager = manager.GetAuctionManager();
    profileManager = manager.GetProfileManager();
}

bool ReddIDP2PManager::RegisterMessageHandlers() {
    if (!running || !node || !node->connman) {
        return false;
    }

    // Store the connman reference
    connman = node->connman.get();

    // Define message handlers for each message type
    messageHandlers[MSG_NAMESPACE_AUCTION_ANNOUNCE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        return this->ProcessNamespaceAuctionAnnounce(pfrom, vRecv, nTimeReceived);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_REQUEST] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        std::string namespaceId;
        vRecv >> namespaceId;
        return this->ProcessNamespaceConfigRequest(pfrom, namespaceId);
    };

    // Add additional message handlers for other message types
    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CNamespaceConfig msg;
        vRecv >> msg;
        return this->ProcessNamespaceConfigResponse(pfrom, msg);
    };
    
    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CAuctionBid msg;
	vRecv >> msg;
	return this->ProcessAuctionBid(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CAuctionFinalize msg;
	vRecv >> msg;
	return this->ProcessAuctionFinalize(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CUserIDAuctionAnnounce msg;
	vRecv >> msg;
	return this->ProcessUserIDAuctionAnnounce(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CReddIDProfileUpdate msg;
	vRecv >> msg;
	return this->ProcessReddIDProfileUpdate(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CReddIDProfileRequest msg;
	vRecv >> msg;
	return this->ProcessReddIDProfileRequest(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
	CReddIDConnection msg;
	vRecv >> msg;
	return this->ProcessReddIDConnection(pfrom, msg);
    };

    messageHandlers[MSG_NAMESPACE_CONFIG_RESPONSE] = [this](CNode* pfrom, CDataStream& vRecv, int64_t nTimeReceived) -> bool {
        CReddIDReputationUpdate msg;
	vRecv >> msg;
	return this->ProcessReddIDReputationUpdate(pfrom, msg);
    };

    LogPrintf("ReddID: Registered message handlers\n");
    return true;
}

void ReddIDP2PManager::ProcessMessage(CNode& pfrom, const std::string& strCommand,
                                     CDataStream& vRecv, int64_t nTimeReceived)
{
    // Early return if we're not running
    if (!running) {
        return;
    }

    LogPrint(BCLog::NET, "ReddID P2P: Processing message %s from peer %d\n", strCommand, pfrom.GetId());

    // Look up the appropriate handler and call it
    auto it = messageHandlers.find(strCommand);
    if (it != messageHandlers.end()) {
        try {
            bool result = it->second(&pfrom, vRecv, nTimeReceived);
            if (!result) {
                LogPrint(BCLog::NET, "ReddID handler returned false for message %s\n", strCommand);
            }
        } catch (const std::exception& e) {
            LogPrint(BCLog::NET, "Error processing ReddID message %s: %s\n", strCommand, e.what());
        }
    } else {
        // No handler found
        LogPrint(BCLog::NET, "Unknown ReddID message type: %s\n", strCommand);
    }
}

bool ReddIDP2PManager::ProcessNamespaceAuctionAnnounce(CNode* pfrom, CDataStream& vRecv, int64_t timestamp) {
    try {
	CNamespaceAuctionAnnounce announce;
        vRecv >> announce;

        LogPrintf("Received namespace auction announcement for %s\n", announce.namespaceId);

        // Validate the namespace ID
        if (!namespaceManager->ValidateNamespaceID(announce.namespaceId)) {
            LogPrintf("Invalid namespace ID: %s\n", announce.namespaceId);
            return false;
        }

        // Check if we already know about this auction
        AuctionInfo auction;
        if (namespaceManager->GetAuctionInfo(announce.auctionId, auction)) {
            LogPrintf("Already know about auction %s\n", announce.auctionId.ToString());
            return true;
        }

        // Create the auction
        auction.auctionId = announce.auctionId;
        auction.namespaceId = announce.namespaceId;
        auction.startTime = announce.startTime;
        auction.endTime = announce.endTime;
        auction.reservePrice = announce.reservePrice;
        auction.type = announce.type;
        auction.state = AUCTION_PENDING;

        // Create a data stream to serialize the message for relay
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << announce;

        // Forward the announcement to other nodes
        RelayMessage(MSG_NAMESPACE_AUCTION_ANNOUNCE, data, {pfrom->GetId()});

        return true;
    } catch (const std::exception& e) {
        LogPrintf("Error processing namespace auction announcement: %s\n", e.what());
        return false;
    }
}

bool ReddIDP2PManager::ProcessNamespaceConfigRequest(CNode* pfrom, const std::string& namespaceId) {
    LogPrintf("Received namespace config request for %s\n", namespaceId);
    
    // Check if we have this namespace
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        LogPrintf("Don't have info for namespace %s\n", namespaceId);
        return false;
    }
    
    // Send the config
    return SendNamespaceConfig(pfrom, namespaceId);
}

bool ReddIDP2PManager::ProcessNamespaceConfigResponse(CNode* pfrom, const CNamespaceConfig& msg) {
    LogPrint(BCLog::NET, "Received namespace config response for %s\n", msg.config.id);

    // Since ValidateNamespaceConfig is private, we need to use an alternative approach
    // Try to use a public method or implement the validation here

    // Method 1: Use an existing public method if available
    // For example, check if the namespace exists or try to retrieve it
    bool isValid = true; // Start with assumption it's valid

    // Check some basic validation rules (implement similar to what ValidateNamespaceConfig would do)
    if (msg.config.id.empty() || msg.config.id.length() > MAX_NAMESPACE_LENGTH) {
        LogPrint(BCLog::NET, "Invalid namespace id: %s\n", msg.config.id);
        isValid = false;
    }

    // Validate percentage distribution
    if (msg.config.burnPct + msg.config.devPct + msg.config.namespaceRevenuePct + msg.config.nodePct != 100) {
        LogPrint(BCLog::NET, "Invalid percentage distribution in namespace config: %s\n", msg.config.id);
        isValid = false;
    }

    // Other validation checks as needed
    
    if (!isValid) {
        LogPrint(BCLog::NET, "Invalid namespace config for %s\n", msg.config.id);
        return false;
    }
    
    // Check if we already have this namespace
    NamespaceInfo existingConfig;
    bool haveNamespace = namespaceManager->GetNamespaceInfo(msg.config.id, existingConfig);

    if (haveNamespace) {
        // We already have this namespace, check if it's more recent
        if (msg.config.lastUpdated <= existingConfig.lastUpdated) {
            LogPrint(BCLog::NET, "Already have more recent config for %s\n", msg.config.id);
            return true;
        }
    }
    
    // Save the config - use a public method like CreateNamespace
    bool success = namespaceManager->CreateNamespace(msg.config);
    
    if (success) {
        // Add pricing tiers
        for (const auto& tier : msg.pricingTiers) {
            namespaceManager->AddPricingTier(msg.config.id, tier.minLength, tier.minPrice);
        }

        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the config to other nodes
        RelayMessage(MSG_NAMESPACE_CONFIG_RESPONSE, data, {pfrom->GetId()});
    }
    
    return success;
}

bool ReddIDP2PManager::ProcessAuctionBid(CNode* pfrom, const CAuctionBid& msg) {
    LogPrintf("Received auction bid for auction %s\n", msg.auctionId.ToString());
    
    // Check if we know about this auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(msg.auctionId, auction)) {
        LogPrintf("Unknown auction %s\n", msg.auctionId.ToString());
        return false;
    }
    
    // Check if the auction is active
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        LogPrintf("Auction %s is not active\n", msg.auctionId.ToString());
        return false;
    }
    
    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the bid to other nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_BID, data, {pfrom->GetId()});
    
    return true;
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

bool ReddIDP2PManager::ProcessUserIDAuctionAnnounce(CNode* pfrom, const CUserIDAuctionAnnounce& msg) {
    LogPrintf("Received user ID auction announcement for %s.%s\n", msg.name, msg.namespaceId);
    
    // Check if we know about this namespace
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(msg.namespaceId, namespaceInfo)) {
        LogPrintf("Unknown namespace %s\n", msg.namespaceId);
        return false;
    }
    
    // Create the auction
    AuctionInfo auction;
    auction.auctionId = msg.auctionId;
    auction.name = msg.name;
    auction.namespaceId = msg.namespaceId;
    auction.startTime = msg.startTime;
    auction.endTime = msg.endTime;
    auction.reservePrice = msg.reservePrice;
    auction.type = msg.type;
    auction.state = AUCTION_PENDING;
    
    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the announcement to other nodes
    RelayMessage(MSG_USERID_AUCTION_ANNOUNCE, data, {pfrom->GetId()});

    return true;
}

bool ReddIDP2PManager::ProcessReddIDProfileUpdate(CNode* pfrom, const CReddIDProfileUpdate& msg) {
    LogPrint(BCLog::NET, "Received profile update for %s\n", msg.reddId);

    // Since ValidateProfile is private, implement simplified validation directly
    bool isValid = true;

    // Check ReddID format - similar logic to ValidateProfile
    if (msg.reddId.empty() || msg.reddId.length() < MIN_REDDID_LENGTH || msg.reddId.length() > MAX_REDDID_LENGTH) {
        LogPrint(BCLog::NET, "Invalid ReddID length: %s\n", msg.reddId);
        isValid = false;
    }

    // Check allowed characters
    for (const char& c : msg.reddId) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            LogPrint(BCLog::NET, "Invalid character in ReddID: %s\n", msg.reddId);
            isValid = false;
            break;
        }
    }

    // Cannot begin or end with underscore
    if (isValid && (msg.reddId[0] == '_' || msg.reddId[msg.reddId.size() - 1] == '_')) {
        LogPrint(BCLog::NET, "ReddID cannot begin or end with underscore: %s\n", msg.reddId);
        isValid = false;
    }

    // Check display name length
    if (isValid && msg.profile.displayName.size() > 64) {
        LogPrint(BCLog::NET, "Display name too long: %s\n", msg.reddId);
        isValid = false;
    }

    // Check bio length
    if (isValid && msg.profile.bio.size() > 256) {
        LogPrint(BCLog::NET, "Bio too long: %s\n", msg.reddId);
        isValid = false;
    }
    
    if (!isValid) {
        LogPrint(BCLog::NET, "Invalid profile for %s\n", msg.reddId);
        return false;
    }
    
    // Check if we already have this profile
    ReddIDProfile existingProfile;
    bool haveProfile = profileManager->GetProfile(msg.reddId, existingProfile);

    if (haveProfile) {
        // We already have this profile, check if it's more recent
        if (msg.profile.lastUpdated <= existingProfile.lastUpdated) {
            LogPrint(BCLog::NET, "Already have more recent profile for %s\n", msg.reddId);
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
    }
    
    return success;
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

bool ReddIDP2PManager::ProcessReddIDConnection(CNode* pfrom, const CReddIDConnection& msg) {
    LogPrint(BCLog::NET, "Received connection from %s to %s\n",
             msg.connection.fromReddId, msg.connection.toReddId);
    
    // Since ValidateConnection is private, implement simplified validation directly
    bool isValid = true;

    // Check that it's not a self-connection
    if (msg.connection.fromReddId == msg.connection.toReddId) {
        LogPrint(BCLog::NET, "Self-connection not allowed from %s\n", msg.connection.fromReddId);
        isValid = false;
    }

    // Check valid connection type
    if (isValid && (msg.connection.connectionType < CONNECTION_FOLLOW ||
                  msg.connection.connectionType > CONNECTION_BLOCK)) {
        LogPrint(BCLog::NET, "Invalid connection type from %s to %s\n",
                 msg.connection.fromReddId, msg.connection.toReddId);
        isValid = false;
    }

    // Check valid visibility
    if (isValid && (msg.connection.visibility < 0 || msg.connection.visibility > 2)) {
        LogPrint(BCLog::NET, "Invalid visibility from %s to %s\n",
                 msg.connection.fromReddId, msg.connection.toReddId);
        isValid = false;
    }

    if (!isValid) {
        LogPrint(BCLog::NET, "Invalid connection from %s to %s\n",
                 msg.connection.fromReddId, msg.connection.toReddId);
        return false;
    }
    
    // Check if the profiles exist
    if (!profileManager->ProfileExists(msg.connection.fromReddId) ||
        !profileManager->ProfileExists(msg.connection.toReddId)) {
        LogPrint(BCLog::NET, "One or both profiles don't exist\n");
        return false;
    }
    
    // Create the connection using a public method
    bool success = profileManager->CreateConnection(msg.connection);
    
    if (success) {
        // Create a data stream to serialize the message
        CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
        data << msg;

        // Forward the connection to other nodes
        RelayMessage(MSG_REDDID_CONNECTION, data, {pfrom->GetId()});
    }
    
    return success;
}

bool ReddIDP2PManager::ProcessReddIDReputationUpdate(CNode* pfrom, const CReddIDReputationUpdate& msg) {
    LogPrintf("Received reputation update for %s\n", msg.reputation.reddId);
    
    // Check if the profile exists
    if (!profileManager->ProfileExists(msg.reputation.reddId)) {
        LogPrintf("Profile %s doesn't exist\n", msg.reputation.reddId);
        return false;
    }
    
    // Check if we already have a more recent reputation score
    ReddIDReputation existingRep;
    if (profileManager->GetReputation(msg.reputation.reddId, existingRep)) {
        if (msg.reputation.lastCalculated <= existingRep.lastCalculated) {
            LogPrintf("Already have more recent reputation for %s\n", msg.reputation.reddId);
            return true;
        }
    }
    
    // Update the reputation
    profileManager->UpdateReputation(msg.reputation);
    
    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << msg;

    // Forward the update to other nodes
    RelayMessage(MSG_REDDID_REPUTATION_UPDATE, data, {pfrom->GetId()});
    
    return true;
}

bool ReddIDP2PManager::AnnounceNamespaceAuction(const AuctionInfo& auction) {
    if (!running) {
        return false;
    }
    
    CNamespaceAuctionAnnounce announce;
    announce.auctionId = auction.auctionId;
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
    RelayMessage(MSG_NAMESPACE_AUCTION_ANNOUNCE, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceNamespaceBid(const BidInfo& bid) {
    if (!running) {
        return false;
    }
    
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
    RelayMessage(MSG_NAMESPACE_AUCTION_BID, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceNamespaceFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice) {
    if (!running) {
        return false;
    }
    
    CAuctionFinalize finalize;
    finalize.auctionId = auctionId;
    finalize.winningBidId = winningBidId;
    finalize.finalPrice = finalPrice;
    finalize.timestamp = GetTime();
    
    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << finalize;
    
    // Relay to all subscribed nodes
    RelayMessage(MSG_NAMESPACE_AUCTION_FINALIZE, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceNamespaceCancel(const uint256& auctionId) {
    if (!running) {
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
    if (!running || !node || !node->connman) {
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
    if (!running || !node || !node->connman) {
        return false;
    }
    
    CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    // Request from first available subscribed node
    bool sentRequest = false;
    node->connman->ForEachNode([&](CNode* pnode) {
        if (sentRequest) {
            return; // Already sent a request
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

bool ReddIDP2PManager::AnnounceUserIDAuction(const AuctionInfo& auction) {
    if (!running) {
        return false;
    }
    
    CUserIDAuctionAnnounce announce;
    announce.auctionId = auction.auctionId;
    announce.name = auction.name;
    announce.namespaceId = auction.namespaceId;
    announce.startTime = auction.startTime;
    announce.endTime = auction.endTime;
    announce.reservePrice = auction.reservePrice;
    announce.type = auction.type;
    announce.timestamp = GetTime();

    // Create a data stream to serialize the message
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << announce;
    
    // Forward the announcement to other nodes
    RelayMessage(MSG_USERID_AUCTION_ANNOUNCE, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceUserIDBid(const BidInfo& bid) {
    if (!running) {
        return false;
    }
    
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
    RelayMessage(MSG_USERID_AUCTION_BID, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceUserIDFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice) {
    if (!running) {
        return false;
    }
    
    CAuctionFinalize finalize;
    finalize.auctionId = auctionId;
    finalize.winningBidId = winningBidId;
    finalize.finalPrice = finalPrice;
    finalize.timestamp = GetTime();

    // Serialize to data stream
    CDataStream data(SER_NETWORK, PROTOCOL_VERSION);
    data << finalize;
    
    // Relay to all subscribed nodes
    RelayMessage(MSG_USERID_AUCTION_FINALIZE, data, {});
    
    return true;
}

bool ReddIDP2PManager::AnnounceUserIDCancel(const uint256& auctionId) {
    if (!running) {
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
    if (!running) {
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
    if (!running) {
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
    if (!running) {
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
    if (!running) {
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
    if (!running || !node || !node->connman) {
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
            return; // Already sent a request
        }

        if (IsNodeSubscribed(pnode->GetId())) {
            node->connman->PushMessage(pnode, msgMaker.Make(MSG_REDDID_PROFILE_REQUEST, data));
            sentRequest = true;
        }
    });
    
    return sentRequest;
}

bool ReddIDP2PManager::AnnounceConnection(const ReddIDConnection& connection) {
    if (!running) {
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
    if (!running) {
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
    subscribedNodes.insert(nodeId);
    return true;
}

bool ReddIDP2PManager::UnsubscribeNode(NodeId nodeId) {
    subscribedNodes.erase(nodeId);
    return true;
}

bool ReddIDP2PManager::IsNodeSubscribed(NodeId nodeId) const {
    return subscribedNodes.find(nodeId) != subscribedNodes.end();
}

void ReddIDP2PManager::RelayMessage(const std::string& command, const CDataStream& data,
                                   const std::vector<NodeId>& exceptNodes) {
    // Check if the node context and connman are available
    if (!node || !node->connman) {
        return;
    }
    
    CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    // Manually apply exception filter and subscription filter
    std::vector<CNode*> nodes_to_send;

    node->connman->ForEachNode([&](CNode* pnode) {
        NodeId nodeId = pnode->GetId();
        
        // Skip if node is in except list
        if (std::find(exceptNodes.begin(), exceptNodes.end(), nodeId) != exceptNodes.end()) {
            return;
        }
        
        // Skip if node is not subscribed
        if (!IsNodeSubscribed(nodeId)) {
            return;
        }
        
        nodes_to_send.push_back(pnode);
    });

    // Send to all filtered nodes
    for (CNode* pnode : nodes_to_send) {
        node->connman->PushMessage(pnode, msgMaker.Make(command, data));
    }
}
