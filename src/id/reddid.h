// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_REDDID_H
#define BITCOIN_ID_REDDID_H

#include <amount.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>
#include <util/system.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
class AuctionManager;
class CBlock;
class CChainState;
class NamespaceManager;
class ProfileManager;
class ReddIDDB;
class ReddIDP2PManager;
struct NodeContext;

/** Whether the ReddID system is enabled when -reddid is not given */
static const bool DEFAULT_ENABLE_REDDID = false;

/**
 * Constants for ReddID operation
 */
static const int MAX_REDDID_LENGTH = 32;
static const int MAX_NAMESPACE_LENGTH = 15;
static const int MIN_REDDID_LENGTH = 3;
static const int MIN_NAMESPACE_LENGTH = 1;
static const CAmount REDDID_MIN_FEE = 5000 * COIN;  // 5,000 RDD
static const CAmount NAMESPACE_MIN_FEE = 10000 * COIN;  // 10,000 RDD

/**
 * OP_RETURN encoding constants
 *
 * Max OP_RETURN script size: MAX_OP_RETURN_RELAY (83 bytes)
 * Script overhead: 1 (OP_RETURN) + 2 (pushdata) = 3 bytes
 * Max payload: 83 - 3 = 80 bytes
 * ReddID header: 1 ('R' prefix) + 1 (opcode) = 2 bytes
 * Max data after header: 80 - 2 = 78 bytes
 *
 * Encoding format (v2 - length-prefixed):
 *   NS Auction Create:  'R' + opcode + len(1) + namespace
 *   NS Auction Bid:     'R' + opcode + auctionId(32) + amount(8)
 *   UserID Create:      'R' + opcode + len(1) + name + len(1) + namespace
 *   UserID Bid:         'R' + opcode + auctionId(32) + amount(8)
 *   Profile Update:     'R' + opcode + len(1) + reddId + profileHash(32)
 *   Connection:         'R' + opcode + len(1) + fromReddId + len(1) + toReddId + type(1)
 */
static const size_t REDDID_OP_RETURN_MAX_PAYLOAD = 80;  // Max bytes in OP_RETURN data push

/**
 * ReddID Operation Codes - These are used in OP_RETURN data to identify the operation
 */
static const unsigned char OP_NAMESPACE_AUCTION_CREATE = '{';
static const unsigned char OP_NAMESPACE_AUCTION_BID = '}';
static const unsigned char OP_NAMESPACE_AUCTION_FINALIZE = '[';
static const unsigned char OP_NAMESPACE_AUCTION_CANCEL = ']';
static const unsigned char OP_AUCTION_CREATE = '%';
static const unsigned char OP_AUCTION_BID = '|';
static const unsigned char OP_AUCTION_FINALIZE = '"';
static const unsigned char OP_AUCTION_CANCEL = '\'';
static const unsigned char OP_REDDID_AUCTION_CREATE = '$';
static const unsigned char OP_REDDID_AUCTION_BID = '^';
static const unsigned char OP_REDDID_AUCTION_FINALIZE = '&';
static const unsigned char OP_REDDID_PROFILE_UPDATE = '#';
static const unsigned char OP_REDDID_CONNECTION = '+';

// Deposit and settlement operation codes
static const unsigned char OP_BID_DEPOSIT = 'D';
static const unsigned char OP_AUCTION_SETTLE = 'S';
static const unsigned char OP_AUCTION_DEFAULT = 'X';
static const unsigned char OP_DEPOSIT_FORFEIT = 'F';

/**
 * Reputation weight constants (Model C: Behavioral + Social Split)
 */
static constexpr double WEIGHT_LONGEVITY = 0.15;
static constexpr double WEIGHT_RELIABILITY = 0.20;
static constexpr double WEIGHT_ENGAGEMENT = 0.20;
static constexpr double WEIGHT_VERIFICATION = 0.20;
static constexpr double WEIGHT_AUCTION = 0.25;
static constexpr int AUTO_VERIFY_ENDORSEMENT_COUNT = 10;
static constexpr double AUTO_VERIFY_MIN_REP = 60.0;
static constexpr double LONGEVITY_DECAY_START_DAYS = 30.0;
static constexpr double LONGEVITY_DECAY_END_DAYS = 330.0;
static constexpr double LONGEVITY_MIN_FACTOR = 0.5;

/**
 * Aggregated auction/deposit statistics for a bidder (used in reputation calculation)
 */
struct BidderStats {
    int totalBids = 0;
    int auctionWins = 0;
    int auctionParticipations = 0;
    int depositsTotal = 0;
    int depositsCompleted = 0;  // RELEASED + CONSUMED
    int depositsForfeit = 0;
    int auctionDefaults = 0;
};

/**
 * Auction state definitions
 */
enum AuctionState : uint8_t {
    AUCTION_PENDING = 0,
    AUCTION_ACTIVE = 1,
    AUCTION_ENDED = 2,
    AUCTION_FINALIZED = 3,
    AUCTION_SETTLEMENT = 4,
    AUCTION_COMPLETED = 5,
    AUCTION_DEFAULTED = 6,
    AUCTION_RE_AUCTION = 7,
    AUCTION_CANCELED = 8
};

/**
 * Deposit state definitions for bid deposits
 */
enum DepositState : uint8_t {
    DEPOSIT_LOCKED_ACTIVE = 0,
    DEPOSIT_RELEASED_LOST = 1,
    DEPOSIT_CONSUMED_WON = 2,
    DEPOSIT_FORFEITED = 3
};

/**
 * Auction types
 */
enum AuctionType : uint8_t {
    AUCTION_STANDARD = 0,
    AUCTION_PREMIUM = 1,
    AUCTION_VERIFIED = 2,
    AUCTION_RENEWAL = 3
};

/**
 * Connection types for ReddID social connections
 */
enum SocialConnectionType : uint8_t {
    CONNECTION_FOLLOW = 0,
    CONNECTION_FRIEND = 1,
    CONNECTION_ENDORSE = 2,
    CONNECTION_BLOCK = 3
};

/**
 * Verification status levels
 */
enum VerificationStatus : uint8_t {
    VERIFICATION_NONE = 0,
    VERIFICATION_SELF = 1,
    VERIFICATION_COMMUNITY = 2,
    VERIFICATION_OFFICIAL = 3
};

/**
 * Represents a namespace in the ReddID system (like ".redd")
 */
class NamespaceInfo {
 public:
    std::string id;                 // Namespace identifier (e.g., "redd")
    CKeyID owner;                   // Owner's key ID
    bool allowNumbers;              // Whether numbers are allowed in user IDs
    bool allowHyphens;              // Whether hyphens are allowed in user IDs
    bool allowUnderscores;          // Whether underscores are allowed in user IDs
    int minLength;                  // Minimum user ID length
    int maxLength;                  // Maximum user ID length
    int renewalPeriod;              // Days until renewal required
    int gracePeriod;                // Days in grace period
    int namespaceRevenuePct;        // % of auctions going to namespace owner
    int burnPct;                    // % of auctions being burned
    int nodePct;                    // % of auctions going to node operators
    int devPct;                     // % of auctions going to development fund
    int minAuctionDuration;         // Minimum auction duration in days
    int maxAuctionDuration;         // Maximum auction duration in days
    double minBidIncrement;         // Minimum bid increment percentage
    uint256 configHash;             // Hash of this configuration
    int64_t lastUpdated;            // When configuration was last updated
    int64_t expiration;             // When namespace ownership expires

    NamespaceInfo() :
        allowNumbers(false),
        allowHyphens(false),
        allowUnderscores(false),
        minLength(3),
        maxLength(32),
        renewalPeriod(365),
        gracePeriod(30),
        namespaceRevenuePct(10),
        burnPct(70),
        nodePct(15),
        devPct(5),
        minAuctionDuration(3),
        maxAuctionDuration(7),
        minBidIncrement(5.0),
        lastUpdated(0),
        expiration(0) {}

    SERIALIZE_METHODS(NamespaceInfo, obj) {
        READWRITE(obj.id);
        READWRITE(obj.owner);
        READWRITE(obj.allowNumbers);
        READWRITE(obj.allowHyphens);
        READWRITE(obj.allowUnderscores);
        READWRITE(obj.minLength);
        READWRITE(obj.maxLength);
        READWRITE(obj.renewalPeriod);
        READWRITE(obj.gracePeriod);
        READWRITE(obj.namespaceRevenuePct);
        READWRITE(obj.burnPct);
        READWRITE(obj.nodePct);
        READWRITE(obj.devPct);
        READWRITE(obj.minAuctionDuration);
        READWRITE(obj.maxAuctionDuration);

        // Fix for double serialization - store as int with 2 decimal precision
        if (ser_action.ForRead()) {
            int32_t minBidIncrementInt;
            READWRITE(minBidIncrementInt);
            const_cast<NamespaceInfo&>(obj).minBidIncrement = minBidIncrementInt / 100.0;
        } else {
            int32_t minBidIncrementInt = static_cast<int32_t>(obj.minBidIncrement * 100.0 + 0.5);  // Round to nearest
            READWRITE(minBidIncrementInt);
        }

        READWRITE(obj.configHash);
        READWRITE(obj.lastUpdated);
        READWRITE(obj.expiration);
    }
};

/**
 * Pricing tier structure for namespace pricing
 */
class PricingTier {
 public:
    std::string namespaceId;      // Namespace identifier
    int minLength;                // Minimum length for this tier
    CAmount minPrice;             // Minimum price for names in this tier

    PricingTier() : minLength(0), minPrice(0) {}

    SERIALIZE_METHODS(PricingTier, obj) {
        READWRITE(obj.namespaceId);
        READWRITE(obj.minLength);
        READWRITE(obj.minPrice);
    }
};

/**
 * Represents a user ID within a namespace (like "alice.redd")
 */
class UserIDInfo {
 public:
    std::string name;              // Name portion of the ID
    std::string namespaceId;       // Namespace portion of the ID
    CKeyID owner;                  // Owner's key ID
    int64_t registrationTime;      // When the ID was registered
    int64_t expirationTime;        // When the ID expires
    int64_t lastTransaction;       // Timestamp of last transaction using this ID
    int transactionCount;          // Number of transactions using this ID
    uint256 metadataHash;          // Hash of additional metadata

    UserIDInfo() :
        registrationTime(0),
        expirationTime(0),
        lastTransaction(0),
        transactionCount(0) {}

    SERIALIZE_METHODS(UserIDInfo, obj) {
        READWRITE(obj.name);
        READWRITE(obj.namespaceId);
        READWRITE(obj.owner);
        READWRITE(obj.registrationTime);
        READWRITE(obj.expirationTime);
        READWRITE(obj.lastTransaction);
        READWRITE(obj.transactionCount);
        READWRITE(obj.metadataHash);
    }

    std::string GetFullName() const {
        return name + "." + namespaceId;
    }
};

/**
 * Represents auction information
 */
class AuctionInfo {
 public:
    uint256 auctionId;            // Unique identifier for the auction
    std::string name;             // Name portion of the ID being auctioned
    std::string namespaceId;      // Namespace portion of the ID
    CKeyID creator;               // Creator's key ID
    int64_t startTime;            // Start time (unix timestamp)
    int64_t endTime;              // End time (unix timestamp)
    CAmount reservePrice;         // Minimum acceptable bid
    CAmount currentBid;           // Highest current bid amount
    CKeyID currentBidder;         // Address of highest bidder
    CAmount depositAmount;        // Required deposit amount
    AuctionState state;           // Current auction state
    AuctionType type;             // Auction type
    uint256 metadataHash;         // Hash of additional metadata
    int blockHeight;              // Block height when auction was created
    uint256 txid;                 // Transaction ID of the auction creation transaction
    int settlementDeadlineHeight; // Block height deadline for settlement (0 = no deadline)
    uint256 finalizeTxid;         // Transaction ID of finalization
    uint256 settleTxid;           // Transaction ID of settlement
    CAmount totalDepositsLocked;  // Total amount of deposits locked for this auction
    int reAuctionCount;           // Number of times this resource has been re-auctioned
    uint256 previousAuctionId;    // Previous auction ID (for re-auctions)

    AuctionInfo() :
        startTime(0),
        endTime(0),
        reservePrice(0),
        currentBid(0),
        depositAmount(0),
        state(AUCTION_PENDING),
        type(AUCTION_STANDARD),
        blockHeight(0),
        settlementDeadlineHeight(0),
        totalDepositsLocked(0),
        reAuctionCount(0) {}

    SERIALIZE_METHODS(AuctionInfo, obj) {
        READWRITE(obj.auctionId);
        READWRITE(obj.name);
        READWRITE(obj.namespaceId);
        READWRITE(obj.creator);
        READWRITE(obj.startTime);
        READWRITE(obj.endTime);
        READWRITE(obj.reservePrice);
        READWRITE(obj.currentBid);
        READWRITE(obj.currentBidder);
        READWRITE(obj.depositAmount);

        // For enums, directly read/write as their underlying integer type
        if (ser_action.ForRead()) {
            uint8_t state_val, type_val;
            READWRITE(state_val);
            READWRITE(type_val);
            const_cast<AuctionInfo&>(obj).state = static_cast<AuctionState>(state_val);
            const_cast<AuctionInfo&>(obj).type = static_cast<AuctionType>(type_val);
        } else {
            uint8_t state_val = static_cast<uint8_t>(obj.state);
            uint8_t type_val = static_cast<uint8_t>(obj.type);
            READWRITE(state_val);
            READWRITE(type_val);
        }

        READWRITE(obj.metadataHash);
        READWRITE(obj.blockHeight);
        READWRITE(obj.txid);
        READWRITE(obj.settlementDeadlineHeight);
        READWRITE(obj.finalizeTxid);
        READWRITE(obj.settleTxid);
        READWRITE(obj.totalDepositsLocked);
        READWRITE(obj.reAuctionCount);
        READWRITE(obj.previousAuctionId);
    }
};

/**
 * Bid information for auctions
 */
class BidInfo {
 public:
    uint256 bidId;                // Unique identifier for the bid
    uint256 auctionId;            // Reference to the auction
    CKeyID bidder;                // Bidder's key ID
    std::string bidderReddId;     // Bidder's ReddID (if they have one)
    CAmount bidAmount;            // Bid amount
    CAmount depositAmount;        // Deposit amount paid
    int64_t bidTime;              // Time when bid was placed
    uint256 proofOfFunds;         // Cryptographic proof of available funds
    uint256 txid;                 // Transaction ID of the bid transaction
    bool isWinner;                // Whether this bid won the auction
    bool refunded;                // Whether the deposit was refunded
    COutPoint depositUtxo;        // UTXO locked as deposit
    int depositLockHeight;        // Block height when deposit was locked
    int depositReleaseHeight;     // Block height when deposit was released
    DepositState depositState;    // Current deposit state

    BidInfo() :
        bidAmount(0),
        depositAmount(0),
        bidTime(0),
        isWinner(false),
        refunded(false),
        depositLockHeight(0),
        depositReleaseHeight(0),
        depositState(DEPOSIT_LOCKED_ACTIVE) {}

    SERIALIZE_METHODS(BidInfo, obj) {
        READWRITE(obj.bidId);
        READWRITE(obj.auctionId);
        READWRITE(obj.bidder);
        READWRITE(obj.bidderReddId);
        READWRITE(obj.bidAmount);
        READWRITE(obj.depositAmount);
        READWRITE(obj.bidTime);
        READWRITE(obj.proofOfFunds);
        READWRITE(obj.txid);
        READWRITE(obj.isWinner);
        READWRITE(obj.refunded);
        READWRITE(obj.depositUtxo);
        READWRITE(obj.depositLockHeight);
        READWRITE(obj.depositReleaseHeight);

        if (ser_action.ForRead()) {
            uint8_t state_val;
            READWRITE(state_val);
            const_cast<BidInfo&>(obj).depositState = static_cast<DepositState>(state_val);
        } else {
            uint8_t state_val = static_cast<uint8_t>(obj.depositState);
            READWRITE(state_val);
        }
    }
};

/**
 * Represents a locked deposit for an auction bid
 */
class DepositLock {
 public:
    COutPoint utxo;               // The locked UTXO
    uint256 auctionId;            // Auction this deposit is for
    uint256 bidId;                // Bid this deposit backs
    CKeyID bidder;                // Bidder who owns the deposit
    CAmount amount;               // Deposit amount
    int lockHeight;               // Block height when locked
    int releaseHeight;            // Block height when released (0 if still locked)
    DepositState state;           // Current deposit state

    DepositLock() :
        amount(0),
        lockHeight(0),
        releaseHeight(0),
        state(DEPOSIT_LOCKED_ACTIVE) {}

    SERIALIZE_METHODS(DepositLock, obj) {
        READWRITE(obj.utxo);
        READWRITE(obj.auctionId);
        READWRITE(obj.bidId);
        READWRITE(obj.bidder);
        READWRITE(obj.amount);
        READWRITE(obj.lockHeight);
        READWRITE(obj.releaseHeight);

        if (ser_action.ForRead()) {
            uint8_t state_val;
            READWRITE(state_val);
            const_cast<DepositLock&>(obj).state = static_cast<DepositState>(state_val);
        } else {
            uint8_t state_val = static_cast<uint8_t>(obj.state);
            READWRITE(state_val);
        }
    }
};

/**
 * Configuration for auction deposit and settlement parameters
 */
class AuctionConfig {
 public:
    std::string configId;                 // Configuration identifier
    int depositPercentage;                // Deposit percentage of bid amount (default 15%)
    int settlementWindowBlocks;           // Blocks allowed for settlement (default 4320 = ~3 days)
    int antiSnipeThresholdSeconds;        // Anti-snipe threshold in seconds
    int antiSnipeExtensionSeconds;        // Anti-snipe extension in seconds
    int minDurationDays;                  // Minimum auction duration in days
    int maxDurationDays;                  // Maximum auction duration in days
    int minBidIncrementPct;               // Minimum bid increment percentage
    std::string communityFundAddress;     // Address for forfeited deposits
    int64_t updatedAt;                    // Last update timestamp

    AuctionConfig() :
        depositPercentage(15),
        settlementWindowBlocks(4320),
        antiSnipeThresholdSeconds(86400),
        antiSnipeExtensionSeconds(21600),
        minDurationDays(3),
        maxDurationDays(7),
        minBidIncrementPct(5),
        updatedAt(0) {}

    static AuctionConfig NamespaceDefaults() {
        AuctionConfig config;
        config.configId = "namespace";
        config.depositPercentage = 20;
        return config;
    }

    static AuctionConfig UserIDDefaults() {
        AuctionConfig config;
        config.configId = "userid";
        config.depositPercentage = 15;
        return config;
    }

    static AuctionConfig ReddIDDefaults() {
        AuctionConfig config;
        config.configId = "reddid";
        config.depositPercentage = 15;
        return config;
    }

    SERIALIZE_METHODS(AuctionConfig, obj) {
        READWRITE(obj.configId);
        READWRITE(obj.depositPercentage);
        READWRITE(obj.settlementWindowBlocks);
        READWRITE(obj.antiSnipeThresholdSeconds);
        READWRITE(obj.antiSnipeExtensionSeconds);
        READWRITE(obj.minDurationDays);
        READWRITE(obj.maxDurationDays);
        READWRITE(obj.minBidIncrementPct);
        READWRITE(obj.communityFundAddress);
        READWRITE(obj.updatedAt);
    }
};

/**
 * ReddID profile information
 */
class ReddIDProfile {
 public:
    std::string reddId;            // The ReddID
    CKeyID owner;                  // Owner's key ID
    std::string displayName;       // User-selected display name
    uint256 avatarHash;            // IPFS hash of avatar image
    std::string bio;               // Short biography or description
    uint256 emailHash;             // Hash of verified email (optional)
    std::string socialData;        // JSON of linked social profiles
    std::vector<unsigned char> messagingPubkey;  // Public key for encrypted messaging
    VerificationStatus verificationStatus;  // Verification level
    int64_t creationTime;          // When the ReddID was created
    int64_t lastUpdated;           // When profile was last updated
    int64_t expirationTime;        // When the ReddID expires
    int64_t lastActivityTime;      // When last meaningful activity occurred (for reputation decay)
    bool active;                   // Whether the ReddID is currently active
    uint8_t flags;                 // Bitfield of profile flags/settings

    ReddIDProfile() :
        verificationStatus(VERIFICATION_NONE),
        creationTime(0),
        lastUpdated(0),
        expirationTime(0),
        lastActivityTime(0),
        active(true),
        flags(0) {}

    SERIALIZE_METHODS(ReddIDProfile, obj) {
        READWRITE(obj.reddId);
        READWRITE(obj.owner);
        READWRITE(obj.displayName);
        READWRITE(obj.avatarHash);
        READWRITE(obj.bio);
        READWRITE(obj.emailHash);
        READWRITE(obj.socialData);
        READWRITE(obj.messagingPubkey);

        // For verification status enum
        if (ser_action.ForRead()) {
            uint8_t status_val;
            READWRITE(status_val);
            const_cast<ReddIDProfile&>(obj).verificationStatus = static_cast<VerificationStatus>(status_val);
        } else {
            uint8_t status_val = static_cast<uint8_t>(obj.verificationStatus);
            READWRITE(status_val);
        }

        READWRITE(obj.creationTime);
        READWRITE(obj.lastUpdated);
        READWRITE(obj.expirationTime);
        READWRITE(obj.lastActivityTime);
        READWRITE(obj.active);
        READWRITE(obj.flags);
    }
};

/**
 * Social connection between ReddIDs
 */
class ReddIDConnection {
 public:
    std::string fromReddId;        // Source ReddID
    std::string toReddId;          // Target ReddID
    SocialConnectionType connectionType;  // Type of connection
    int64_t creationTime;          // When connection was established
    int64_t lastInteraction;       // When last interaction occurred
    int visibility;                // Privacy setting (0=public, 1=friends, 2=private)
    std::string metadata;          // Additional connection metadata
    uint256 txid;                  // Transaction ID of the connection operation

    ReddIDConnection() :
        connectionType(CONNECTION_FOLLOW),
        creationTime(0),
        lastInteraction(0),
        visibility(0) {}

    SERIALIZE_METHODS(ReddIDConnection, obj) {
        READWRITE(obj.fromReddId);
        READWRITE(obj.toReddId);

        // For connection type enum
        if (ser_action.ForRead()) {
            uint8_t type_val;
            READWRITE(type_val);
            const_cast<ReddIDConnection&>(obj).connectionType = static_cast<SocialConnectionType>(type_val);
        } else {
            uint8_t type_val = static_cast<uint8_t>(obj.connectionType);
            READWRITE(type_val);
        }

        READWRITE(obj.creationTime);
        READWRITE(obj.lastInteraction);
        READWRITE(obj.visibility);
        READWRITE(obj.metadata);
        READWRITE(obj.txid);
    }
};

/**
 * Reputation information for ReddIDs
 */
class ReddIDReputation {
 public:
    std::string reddId;            // The ReddID
    double overallScore;           // Aggregate reputation score (0-100)
    double longevityScore;         // Score component for account age with activity decay
    double reliabilityScore;       // Score component for deposit completion rate
    double engagementScore;        // Score component for differentiated social connections
    double verificationScore;      // Score component for verification tier + endorsement bonus
    double auctionScore;           // Score component for auction participation and wins
    int64_t lastCalculated;        // When the score was last calculated
    uint256 calculationProof;      // Proof of calculation correctness
    std::string calculatorSignatures;  // JSON of node signatures validating the calculation

    ReddIDReputation() :
        overallScore(0),
        longevityScore(0),
        reliabilityScore(0),
        engagementScore(0),
        verificationScore(0),
        auctionScore(0),
        lastCalculated(0) {}

    SERIALIZE_METHODS(ReddIDReputation, obj) {
        READWRITE(obj.reddId);

        // Fix for double serialization - store scores as int with 2 decimal precision
        if (ser_action.ForRead()) {
            int32_t overallScoreInt, longevityScoreInt, reliabilityScoreInt;
            int32_t engagementScoreInt, verificationScoreInt, auctionScoreInt;

            READWRITE(overallScoreInt);
            READWRITE(longevityScoreInt);
            READWRITE(reliabilityScoreInt);
            READWRITE(engagementScoreInt);
            READWRITE(verificationScoreInt);
            READWRITE(auctionScoreInt);

            auto& mutable_obj = const_cast<ReddIDReputation&>(obj);
            mutable_obj.overallScore = overallScoreInt / 100.0;
            mutable_obj.longevityScore = longevityScoreInt / 100.0;
            mutable_obj.reliabilityScore = reliabilityScoreInt / 100.0;
            mutable_obj.engagementScore = engagementScoreInt / 100.0;
            mutable_obj.verificationScore = verificationScoreInt / 100.0;
            mutable_obj.auctionScore = auctionScoreInt / 100.0;
        } else {
            int32_t overallScoreInt = static_cast<int32_t>(obj.overallScore * 100.0 + 0.5);
            int32_t longevityScoreInt = static_cast<int32_t>(obj.longevityScore * 100.0 + 0.5);
            int32_t reliabilityScoreInt = static_cast<int32_t>(obj.reliabilityScore * 100.0 + 0.5);
            int32_t engagementScoreInt = static_cast<int32_t>(obj.engagementScore * 100.0 + 0.5);
            int32_t verificationScoreInt = static_cast<int32_t>(obj.verificationScore * 100.0 + 0.5);
            int32_t auctionScoreInt = static_cast<int32_t>(obj.auctionScore * 100.0 + 0.5);

            READWRITE(overallScoreInt);
            READWRITE(longevityScoreInt);
            READWRITE(reliabilityScoreInt);
            READWRITE(engagementScoreInt);
            READWRITE(verificationScoreInt);
            READWRITE(auctionScoreInt);
        }

        READWRITE(obj.lastCalculated);
        READWRITE(obj.calculationProof);
        READWRITE(obj.calculatorSignatures);
    }
};

/**
 * Main ReddID manager class
 */
class ReddIDManager {
 private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    NodeContext* node;

    CChainState* chainstate;  // Reference to the chain state

    // Component managers
    std::unique_ptr<ReddIDDB> reddidDB;
    std::unique_ptr<NamespaceManager> namespaceManager;
    std::unique_ptr<AuctionManager> auctionManager;
    std::unique_ptr<ProfileManager> profileManager;

 public:
    ReddIDManager(NodeContext& nodeIn);
    ~ReddIDManager();

    // Node lifecycle methods
    bool Init();
    bool Start();
    void Interrupt();
    bool Stop();
    bool IsRunning() const { return m_running; }
    bool IsInitialized() const { return m_initialized; }

    // Accessor methods for the component managers (internal use)
    ReddIDDB* GetReddIDDB() const { return reddidDB.get(); }
    NamespaceManager* GetNamespaceManager() const { return namespaceManager.get(); }
    AuctionManager* GetAuctionManager() const { return auctionManager.get(); }
    ProfileManager* GetProfileManager() const { return profileManager.get(); }
    // There is no P2P manager yet, so the component managers are handed a null pointer
    ReddIDP2PManager* GetP2PManager() const { return nullptr; }

    CChainState* GetChainState() const { return chainstate; }

    // Expiration operations
    bool ExpireUserID(const std::string& name, const std::string& namespaceId);
    void CheckExpiredUserIDs();

    // The node calls ProcessTransaction and UndoBlock with cs_main held, from
    // CChainState::ConnectTip and CChainState::DisconnectTip. They are not
    // annotated as requiring the lock, because unit tests call them without it.

    // Block undo operations (reorg safety)
    bool UndoBlock(const CBlock& block, int nHeight);
    bool UndoTransaction(const CTransaction& tx, int nHeight);

    // Core operations
    bool ProcessTransaction(const CTransaction& tx, int nHeight);
    bool ValidateReddID(const std::string& reddId);
    bool ValidateNamespace(const std::string& namespaceId);
    bool ValidateUserID(const std::string& name, const std::string& namespaceId);

    // User ID operations
    bool CreateUserIDAuction(const std::string& name, const std::string& namespaceId,
                           const CKeyID& creator, CAmount reservePrice, int durationDays,
                           AuctionType type, uint256& auctionId);
    bool FinalizeUserIDAuction(const uint256& auctionId);
    bool CancelUserIDAuction(const uint256& auctionId, const CKeyID& creator);
    bool TransferUserID(const std::string& name, const std::string& namespaceId,
                       const CKeyID& from, const CKeyID& to);
    bool RenewUserID(const std::string& name, const std::string& namespaceId, const CKeyID& owner);

    // ReddID profile operations
    bool CalculateReputation(const std::string& reddId, ReddIDReputation& result);

    // Getters
    bool GetUserIDInfo(const std::string& name, const std::string& namespaceId, UserIDInfo& result);

    // Auction discovery
    std::vector<BidInfo> GetAuctionBids(const uint256& auctionId);

    // Validation helper functions
    CAmount CalculateMinPriceForNamespace(const std::string& namespaceId);
    CAmount CalculateMinPriceForUserID(const std::string& name, const std::string& namespaceId);
    CAmount CalculateRenewalFee(const std::string& name, const std::string& namespaceId);

    // OP_RETURN helpers. These forward to the free functions of the same
    // names and are defined in id/transaction_handler.cpp.
    bool ParseReddIDOpReturn(const CScript& script, unsigned char& opCode,
                           std::vector<unsigned char>& data);
    CScript CreateNamespaceAuctionOpReturn(const std::string& namespaceId);
    CScript CreateUserIDAuctionOpReturn(const std::string& name,
                                      const std::string& namespaceId);
    CScript CreateUserIDBidOpReturn(const uint256& auctionId, CAmount bidAmount);
    CScript CreateProfileUpdateOpReturn(const std::string& reddId, const uint256& profileHash);
    CScript CreateConnectionOpReturn(const std::string& fromReddId,
                                   const std::string& toReddId,
                                   SocialConnectionType type);
    CScript CreateBidDepositOpReturn(const uint256& auctionId, CAmount bidAmount,
                                    uint8_t depositVout, const CKeyID& bidderPKH);
    CScript CreateAuctionSettleOpReturn(const uint256& auctionId, const uint256& bidId);
};

#endif // BITCOIN_ID_REDDID_H
