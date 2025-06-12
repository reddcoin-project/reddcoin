// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_REDDID_P2P_H
#define BITCOIN_ID_REDDID_P2P_H

#include <id/reddid.h>
#include <net.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <serialize.h>
#include <uint256.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>


// Constants for P2P message handling
static const size_t MAX_REDDID_MESSAGE_SIZE = 1 * 1024 * 1024;  // 1 MB maximum message size
static const int DEFAULT_MAX_MESSAGES_PER_MINUTE = 20;
static const int MAX_AUCTION_ANNOUNCES_PER_MINUTE = 10;
static const int MAX_AUCTION_BIDS_PER_MINUTE = 20;
static const int MAX_PROFILE_UPDATES_PER_MINUTE = 5;
static const int MAX_CONNECTIONS_PER_MINUTE = 15;
static const int RATE_LIMIT_WINDOW_SECONDS = 60;

/**
 * ReddID P2P message types
 */
static const char* MSG_NAMESPACE_AUCTION_ANNOUNCE = "nsaucann";
static const char* MSG_NAMESPACE_AUCTION_BID = "nsaucbid";
static const char* MSG_NAMESPACE_AUCTION_FINALIZE = "nsaucfin";
static const char* MSG_NAMESPACE_AUCTION_CANCEL = "nsauccan";
static const char* MSG_NAMESPACE_CONFIG_REQUEST = "nsconreq";
static const char* MSG_NAMESPACE_CONFIG_RESPONSE = "nsconres";
static const char* MSG_USERID_AUCTION_ANNOUNCE = "uidaucann";
static const char* MSG_USERID_AUCTION_BID = "uidaucbid";
static const char* MSG_USERID_AUCTION_FINALIZE = "uidaucfin";
static const char* MSG_USERID_AUCTION_CANCEL = "uidauccan";
static const char* MSG_REDDID_AUCTION_ANNOUNCE = "ridaucann";
static const char* MSG_REDDID_AUCTION_BID = "ridaucbid";
static const char* MSG_REDDID_AUCTION_FINALIZE = "ridaucfin";
static const char* MSG_REDDID_PROFILE_UPDATE = "ridprofup";
static const char* MSG_REDDID_CONNECTION = "ridconn";
static const char* MSG_REDDID_PROFILE_REQUEST = "ridprofreq";
static const char* MSG_REDDID_PROFILE_RESPONSE = "ridprofres";
static const char* MSG_REDDID_REPUTATION_UPDATE = "ridrep";

/**
 * P2P message structures for ReddID
 */

/**
 * Base class for all ReddID messages
 */
class CReddIDMessageBase {
 public:
    int64_t timestamp;

    CReddIDMessageBase() : timestamp(0) {}

    SERIALIZE_METHODS(CReddIDMessageBase, obj) {
        READWRITE(obj.timestamp);
    }
};

// Message handler function type
using ReddIDMessageHandlerFn = std::function<bool(CNode*, CDataStream&, int64_t)>;

/**
 * Namespace auction announcement message
 */
class CNamespaceAuctionAnnounce : public CReddIDMessageBase {
 public:
    uint256 auctionId;
    std::string namespaceId;
    CKeyID creator;        // Add this field
    int64_t startTime;
    int64_t endTime;
    CAmount reservePrice;
    AuctionType type;
    uint256 configHash;

    SERIALIZE_METHODS(CNamespaceAuctionAnnounce, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.auctionId);
        READWRITE(obj.namespaceId);
        READWRITE(obj.creator);    // Add serialization for creator
        READWRITE(obj.startTime);
        READWRITE(obj.endTime);
        READWRITE(obj.reservePrice);

        // Direct serialization of AuctionType enum
        if (ser_action.ForRead()) {
            uint8_t type_val;
            READWRITE(type_val);
            const_cast<CNamespaceAuctionAnnounce&>(obj).type = static_cast<AuctionType>(type_val);
        } else {
            uint8_t type_val = static_cast<uint8_t>(obj.type);
            READWRITE(type_val);
        }

        READWRITE(obj.configHash);
    }
};

/**
 * Namespace configuration message
 */
class CNamespaceConfig : public CReddIDMessageBase {
 public:
    NamespaceInfo config;
    std::vector<PricingTier> pricingTiers;

    SERIALIZE_METHODS(CNamespaceConfig, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.config);
        READWRITE(obj.pricingTiers);
    }
};

/**
 * Auction bid message
 */
class CAuctionBid : public CReddIDMessageBase {
 public:
    uint256 auctionId;
    uint256 bidId;
    CKeyID bidder;         // Add this field to include bidder information
    CAmount bidAmount;
    CAmount depositAmount;

    SERIALIZE_METHODS(CAuctionBid, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.auctionId);
        READWRITE(obj.bidId);
        READWRITE(obj.bidder);     // Add serialization for bidder
        READWRITE(obj.bidAmount);
        READWRITE(obj.depositAmount);
    }
};

/**
 * Auction finalization message
 */
class CAuctionFinalize : public CReddIDMessageBase {
 public:
    uint256 auctionId;
    uint256 winningBidId;
    CAmount finalPrice;
    CKeyID creator;       // Add creator field for ownership verification

    SERIALIZE_METHODS(CAuctionFinalize, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.auctionId);
        READWRITE(obj.winningBidId);
        READWRITE(obj.finalPrice);
        READWRITE(obj.creator);    // Add serialization for creator
    }
};

/**
 * User ID auction announcement message
 */
class CUserIDAuctionAnnounce : public CReddIDMessageBase {
 public:
    uint256 auctionId;
    std::string name;
    std::string namespaceId;
    CKeyID creator;        // Add this field
    int64_t startTime;
    int64_t endTime;
    CAmount reservePrice;
    AuctionType type;

    SERIALIZE_METHODS(CUserIDAuctionAnnounce, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.auctionId);
        READWRITE(obj.name);
        READWRITE(obj.namespaceId);
        READWRITE(obj.creator);    // Add serialization for creator
        READWRITE(obj.startTime);
        READWRITE(obj.endTime);
        READWRITE(obj.reservePrice);

        // Direct serialization of AuctionType enum
        if (ser_action.ForRead()) {
            uint8_t type_val;
            READWRITE(type_val);
            const_cast<CUserIDAuctionAnnounce&>(obj).type = static_cast<AuctionType>(type_val);
        } else {
            uint8_t type_val = static_cast<uint8_t>(obj.type);
            READWRITE(type_val);
        }
    }
};

/**
 * ReddID profile update message
 */
class CReddIDProfileUpdate : public CReddIDMessageBase {
 public:
    std::string reddId;
    uint256 profileHash;
    ReddIDProfile profile;

    SERIALIZE_METHODS(CReddIDProfileUpdate, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.reddId);
        READWRITE(obj.profileHash);
        READWRITE(obj.profile);
    }
};

/**
 * ReddID profile request message
 */
class CReddIDProfileRequest : public CReddIDMessageBase {
 public:
    std::string reddId;

    SERIALIZE_METHODS(CReddIDProfileRequest, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.reddId);
    }
};

/**
 * ReddID connection message
 */
class CReddIDConnection : public CReddIDMessageBase {
 public:
    ReddIDConnection connection;

    SERIALIZE_METHODS(CReddIDConnection, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.connection);
    }
};

/**
 * ReddID reputation update message
 */
class CReddIDReputationUpdate : public CReddIDMessageBase {
 public:
    ReddIDReputation reputation;

    SERIALIZE_METHODS(CReddIDReputationUpdate, obj) {
        READWRITEAS(CReddIDMessageBase, obj);
        READWRITE(obj.reputation);
    }
};

/**
 * ReddID P2P Manager class with enhanced Bitcoin v22 integration
 */
class ReddIDP2PManager {
 private:
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};

    ReddIDManager* reddIDManager;  // Reference to parent ReddIDManager
    NodeContext* node;  // Pointer to the NodeContext
    CConnman* connman;  // Pointer to the connection manager
    NamespaceManager* namespaceManager;  // Pointer to the Namespace Manager
    ProfileManager* profileManager;      // Pointer to the Profile Manager
    AuctionManager* auctionManager;      // Pointer to the Auction Manager

    // Thread-safe containers with their mutexes
    mutable RecursiveMutex cs_subscriptions;
    std::set<NodeId> subscribedNodes GUARDED_BY(cs_subscriptions);
    std::set<NodeId> reddidCapableNodes GUARDED_BY(cs_subscriptions);

    mutable RecursiveMutex cs_message_rates;
    std::map<NodeId, std::map<std::string, std::pair<int64_t, int>>> m_messageRateLimits GUARDED_BY(cs_message_rates);

    // Store registered message handlers
    std::map<std::string, ReddIDMessageHandlerFn> messageHandlers;

    // Message handling methods
    bool ProcessNamespaceAuctionAnnounce(CNode* pfrom, CDataStream& vRecv, int64_t timestamp);
    bool ProcessNamespaceConfigRequest(CNode* pfrom, const std::string& namespaceId);
    bool ProcessNamespaceConfigResponse(CNode* pfrom, const CNamespaceConfig& msg);
    bool ProcessAuctionBid(CNode* pfrom, const CAuctionBid& msg);
    bool ProcessAuctionFinalize(CNode* pfrom, const CAuctionFinalize& msg);
    bool ProcessNamespaceAuctionCancel(CNode* pfrom, const uint256& auctionId);
    bool ProcessUserIDAuctionAnnounce(CNode* pfrom, const CUserIDAuctionAnnounce& msg);
    bool ProcessUserIDAuctionBid(CNode* pfrom, const CAuctionBid& msg);
    bool ProcessUserIDAuctionFinalize(CNode* pfrom, const CAuctionFinalize& msg);
    bool ProcessUserIDAuctionCancel(CNode* pfrom, const uint256& auctionId);
    bool ProcessReddIDProfileUpdate(CNode* pfrom, const CReddIDProfileUpdate& msg);
    bool ProcessReddIDProfileRequest(CNode* pfrom, const CReddIDProfileRequest& msg);
    bool ProcessReddIDProfileResponse(CNode* pfrom, const CReddIDProfileUpdate& msg);
    bool ProcessReddIDConnection(CNode* pfrom, const CReddIDConnection& msg);
    bool ProcessReddIDReputationUpdate(CNode* pfrom, const CReddIDReputationUpdate& msg);

    bool CheckMessageRate(CNode* pfrom, const std::string& strCommand);
    void CleanupRateLimitData();

    // Helper method to check ReddID compatibility
    bool NodeSupportsReddID(const CNode* pnode) const {
        return pnode && (pnode->GetLocalServices() & NODE_REDDID);
    }

 public:
    ReddIDP2PManager(NodeContext& nodeIn);
    ~ReddIDP2PManager();

    // Initialize and lifecycle methods
    bool Init(ReddIDManager* manager);
    bool Start();
    void Interrupt();
    bool Stop();
    bool IsInitialized() const { return m_initialized; }
    bool IsRunning() const { return m_running; }

    // Message handler registration
    bool RegisterMessageHandlers();

    // Message handling methods
    void ProcessMessage(CNode& pfrom, const std::string& strCommand,
                          CDataStream& vRecv, int64_t nTimeReceived);

    // Message announcement methods (sending)
    bool AnnounceNamespaceAuction(const AuctionInfo& auction);
    bool AnnounceNamespaceBid(const BidInfo& bid);
//    bool AnnounceNamespaceFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice);
    bool AnnounceNamespaceCancel(const uint256& auctionId);
    bool SendNamespaceConfig(CNode* pfrom, const std::string& namespaceId);
    bool RequestNamespaceConfig(const std::string& namespaceId);
    bool RequestAllNamespaceConfigs(CNode* targetNode = nullptr);

    bool AnnounceUserIDAuction(const AuctionInfo& auction);
    bool AnnounceUserIDBid(const BidInfo& bid);
//    bool AnnounceUserIDFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice);
    bool AnnounceUserIDCancel(const uint256& auctionId);

    bool AnnounceReddIDAuction(const AuctionInfo& auction);
    bool AnnounceReddIDBid(const BidInfo& bid);
    bool AnnounceReddIDFinalize(const uint256& auctionId, const uint256& winningBidId, CAmount finalPrice);
    bool AnnounceProfileUpdate(const std::string& reddId, const ReddIDProfile& profile);
    bool RequestProfile(const std::string& reddId);
    bool AnnounceConnection(const ReddIDConnection& connection);
    bool AnnounceReputationUpdate(const ReddIDReputation& reputation);

    // Node subscription management for efficient message relay
    bool SubscribeNode(NodeId nodeId);
    bool UnsubscribeNode(NodeId nodeId);
    bool IsNodeSubscribed(NodeId nodeId) const;
    std::vector<NodeId> GetSubscribedNodes() const;

    // Network event handlers
    void OnNodeConnected(CNode* pnode);
    void OnNodeDisconnected(NodeId nodeId);

    // Message relay helper
    void RelayMessage(const std::string& command, const CDataStream& data,
                     const std::vector<NodeId>& exceptNodes = {});

    /**
     * Get all connected nodes that support ReddID
     * @return Vector of NodeIds for ReddID-capable peers
     */
    std::vector<NodeId> GetReddIDNodes() const;

    /**
     * Check if we have any ReddID-capable peers
     * @return true if at least one peer supports ReddID
     */
    bool HasReddIDPeers() const;

    /**
     * Check if this node is advertising ReddID support
     * @return true if NODE_REDDID is set in local services
     */
    bool IsReddIDEnabled() const;
};

#endif  // BITCOIN_ID_REDDID_P2P_H
