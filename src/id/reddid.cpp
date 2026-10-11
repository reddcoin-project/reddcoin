// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/reddid.h>
#include <id/namespace.h>
#include <id/auction.h>
#include <id/profile.h>
#include <id/reddid_db.h>

#include <logging.h>
#include <node/context.h>
#include <scheduler.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <vector>

ReddIDManager::ReddIDManager(NodeContext& nodeIn)
    : m_initialized(false),
      m_running(false),
      chainstate(nullptr),
      node(&nodeIn) {
    // Components will be initialized in Init()
}

ReddIDManager::~ReddIDManager() {
    Stop();
}

bool ReddIDManager::Init() {
    LogPrintf("Initializing ReddID manager\n");

    // Prevent double initialization
    if (m_initialized) {
        LogPrintf("ReddID manager already initialized\n");
        return true;
    }

    // Parse configuration options
    bool enableReddID = gArgs.GetBoolArg("-reddid", DEFAULT_ENABLE_REDDID);
    if (!enableReddID) {
        LogPrintf("ReddID system disabled\n");
        return true;
    }

    // Initialize database
    size_t nCacheSize = gArgs.GetArg("-reddidbcache", 8);
    bool fMemory = gArgs.GetBoolArg("-reddidbmemory", false);
    bool fWipe = gArgs.GetBoolArg("-reddidbwipe", false);

    LogPrintf("ReddID: Using %u MB for database cache\n", nCacheSize);

    // Initialize managers
    try {
        // Initialize database
        reddidDB = std::make_unique<ReddIDDB>(nCacheSize, fMemory, fWipe);

        // Create other managers
        namespaceManager = std::make_unique<NamespaceManager>(*this);
        auctionManager = std::make_unique<AuctionManager>(*this);
        profileManager = std::make_unique<ProfileManager>(*this);

        // Initialize chainstate reference
        if (node && node->chainman) {
            chainstate = &node->chainman->ActiveChainstate();
            LogPrintf("ReddID manager using chainstate at height %d\n",
                     chainstate->m_chain.Height());
        } else {
            LogPrintf("Warning: ReddID manager initialized without chainstate access\n");
        }

        // Initialize mmanagers in order
        if (!namespaceManager->Init(this)) {
            LogPrintf("Warning: Failed to load namespace data\n");
        }

        if (!auctionManager->Init(this)) {
            LogPrintf("Warning: Failed to initialize auction manager\n");
        }

        if (!profileManager->Init(this)) {
            LogPrintf("Warning: Failed to initialize profile manager\n");
        }

        if (auctionManager && chainstate) {
            auctionManager->SetChainState(chainstate);
        }

        // Set initialization flag to true
        m_initialized = true;
    } catch (const std::exception& e) {
        LogPrintf("Failed to initialize ReddID components: %s\n", e.what());
        return false;
    }

    return true;
}

bool ReddIDManager::Start() {
    LogPrintf("Starting ReddID manager\n");

    if (m_running) {
        return true;
    }

    // Should check initialization
    if (!m_initialized) {
        LogPrintf("ERROR: ReddIDManager::Start: Manager not initialized\n");
        return false;
    }

    if (!namespaceManager->Start()) {
        LogPrintf("Failed to start namespace manager\n");
        return false;
    }

    if (!auctionManager->Start()) {
        LogPrintf("Failed to start auction manager\n");
        return false;
    }

    if (!profileManager->Start()) {
        LogPrintf("Failed to start profile manager\n");
        return false;
    }

    // Schedule periodic tasks
    if (node && node->scheduler) {
        node->scheduler->scheduleEvery([this]() {
            // Check for expired auctions
            auctionManager->CheckExpiredAuctions();
            // Check for expired namespaces (cascades to user IDs)
            namespaceManager->CheckExpiredNamespaces();
            // Check for expired user IDs (standalone expiration)
            CheckExpiredUserIDs();
            // Check for expired profiles
            profileManager->CheckExpiredProfiles();
            return true;
        }, std::chrono::milliseconds{60000});  // Check every minute
    } else {
        LogPrintf("Warning: Scheduler not available, periodic tasks disabled\n");
    }

    m_running = true;
    return true;
}

void ReddIDManager::Interrupt() {
    LogPrintf("Interrupting ReddID manager\n");

    // Should check initialization
    if (!m_initialized) {
        LogPrintf("ERROR: ReddIDManager::Interrupt: Manager not initialized\n");
        return;
    }

    if (namespaceManager) namespaceManager->Interrupt();
    if (auctionManager) auctionManager->Interrupt();
    if (profileManager) profileManager->Interrupt();

    // Flag as not running, but don't clean up yet
    m_running = false;
}

bool ReddIDManager::Stop() {
    if (!m_running) {
        return true;
    }

    LogPrintf("Stopping ReddID manager\n");

    // Save data
    if (namespaceManager) {
        namespaceManager->Stop();
    }

    if (auctionManager) {
        auctionManager->Stop();
    }

    if (profileManager) {
        profileManager->Stop();
    }

    // Clear member pointers
    profileManager.reset();
    auctionManager.reset();
    namespaceManager.reset();
    reddidDB.reset();

    m_running = false;
    m_initialized = false;

    return true;
}

bool ReddIDManager::UndoBlock(const CBlock& block, int nHeight) {
    // Iterate transactions in reverse order to undo state changes correctly
    for (int i = (int)block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = *block.vtx[i];
        if (tx.IsCoinBase() || tx.IsCoinStake()) continue;

        UndoTransaction(tx, nHeight);
    }
    return true;
}

bool ReddIDManager::UndoTransaction(const CTransaction& tx, int nHeight) {
    bool processed = false;

    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        unsigned char opCode;
        std::vector<unsigned char> data;

        if (ParseReddIDOpReturn(txout.scriptPubKey, opCode, data)) {
            switch (opCode) {
                // Namespace operations
                case OP_NAMESPACE_AUCTION_CREATE:
                case OP_NAMESPACE_AUCTION_BID:
                case OP_NAMESPACE_AUCTION_FINALIZE:
                case OP_NAMESPACE_AUCTION_CANCEL:
                    processed = namespaceManager->UndoTransaction(tx, nHeight);
                    break;

                // User ID / auction operations
                case OP_AUCTION_CREATE:
                case OP_AUCTION_BID:
                case OP_AUCTION_FINALIZE:
                case OP_AUCTION_CANCEL:
                case OP_BID_DEPOSIT:
                case OP_AUCTION_SETTLE:
                case OP_AUCTION_DEFAULT:
                case OP_DEPOSIT_FORFEIT:
                    processed = auctionManager->UndoTransaction(tx, nHeight);
                    break;

                // Profile operations
                case OP_REDDID_AUCTION_CREATE:
                case OP_REDDID_AUCTION_BID:
                case OP_REDDID_AUCTION_FINALIZE:
                case OP_REDDID_PROFILE_UPDATE:
                case OP_REDDID_CONNECTION:
                    processed = profileManager->UndoTransaction(tx, nHeight);
                    break;

                default:
                    break;
            }

            if (processed) break;
        }
    }

    return true;
}

bool ReddIDManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    // Ignore coinbase and coinstake transactions
    if (tx.IsCoinBase() || tx.IsCoinStake()) {
        return true;
    }

    bool processed = false;

    // Process each output to look for OP_RETURN with ReddID data
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        unsigned char opCode;
        std::vector<unsigned char> data;

        if (ParseReddIDOpReturn(txout.scriptPubKey, opCode, data)) {
            switch (opCode) {
                // Namespace operations
                case OP_NAMESPACE_AUCTION_CREATE:
                    processed = namespaceManager->ProcessTransaction(tx, nHeight);
                    break;
                case OP_NAMESPACE_AUCTION_BID:
                case OP_NAMESPACE_AUCTION_FINALIZE:
                case OP_NAMESPACE_AUCTION_CANCEL:
                    processed = namespaceManager->ProcessTransaction(tx, nHeight);
                    break;

                // User ID operations
                case OP_AUCTION_CREATE:
                case OP_AUCTION_BID:
                case OP_AUCTION_FINALIZE:
                case OP_AUCTION_CANCEL:
                // Deposit and settlement operations
                case OP_BID_DEPOSIT:
                case OP_AUCTION_SETTLE:
                case OP_AUCTION_DEFAULT:
                case OP_DEPOSIT_FORFEIT:
                    processed = auctionManager->ProcessTransaction(tx, nHeight);
                    break;

                // ReddID operations
                case OP_REDDID_AUCTION_CREATE:
                case OP_REDDID_AUCTION_BID:
                case OP_REDDID_AUCTION_FINALIZE:
                case OP_REDDID_PROFILE_UPDATE:
                case OP_REDDID_CONNECTION:
                    processed = profileManager->ProcessTransaction(tx, nHeight);
                    break;

                default:
                    // Not a ReddID operation
                    break;
            }

            if (processed) {
                break;  // Found and processed a ReddID operation
            }
        }
    }

    return true;
}

bool ReddIDManager::ValidateReddID(const std::string& reddId) {
    // Check length
    if (reddId.size() < MIN_REDDID_LENGTH || reddId.size() > MAX_REDDID_LENGTH) {
        return false;
    }

    // Check allowed characters (a-z, 0-9, _)
    for (const char& c : reddId) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }

    // Cannot begin or end with underscore
    if (reddId[0] == '_' || reddId[reddId.size() - 1] == '_') {
        return false;
    }

    return true;
}

bool ReddIDManager::ValidateNamespace(const std::string& namespaceId) {
    return namespaceManager->ValidateNamespaceID(namespaceId);
}

bool ReddIDManager::ValidateUserID(const std::string& name, const std::string& namespaceId) {
    // First validate the namespace
    if (!ValidateNamespace(namespaceId)) {
        return false;
    }

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return false;
    }

    // Check length
    if (name.size() < static_cast<size_t>(namespaceInfo.minLength) || name.size() > static_cast<size_t>(namespaceInfo.maxLength)) {
        return false;
    }

    // Check allowed characters based on namespace settings
    for (const char& c : name) {
        bool isValid = (c >= 'a' && c <= 'z');  // Letters always allowed

        // Check if numbers are allowed
        if (namespaceInfo.allowNumbers && (c >= '0' && c <= '9')) {
            isValid = true;
        }

        // Check if hyphens are allowed
        if (namespaceInfo.allowHyphens && c == '-') {
            isValid = true;
        }

        // Check if underscores are allowed
        if (namespaceInfo.allowUnderscores && c == '_') {
            isValid = true;
        }

        if (!isValid) {
            return false;
        }
    }

    // Cannot begin or end with hyphen
    if (name[0] == '-' || name[name.size() - 1] == '-') {
        return false;
    }

    return true;
}

bool ReddIDManager::CreateUserIDAuction(const std::string& name, const std::string& namespaceId,
                                      const CKeyID& creator, CAmount reservePrice, int durationDays,
                                      AuctionType type, uint256& auctionId) {
    if (!m_initialized || !namespaceManager || !auctionManager) {
        LogPrintf("ERROR: ReddIDManager::CreateUserIDAuction: Manager not initialized\n");
        return false;
    }

    // Validate user ID
    if (!ValidateUserID(name, namespaceId)) {
        return false;
    }

    // Create AuctionInfo
    AuctionInfo auction;
    auction.name = name;
    auction.namespaceId = namespaceId;
    auction.creator = creator;
    auction.reservePrice = reservePrice;
    auction.startTime = GetTime();
    auction.endTime = auction.startTime + (durationDays * 24 * 60 * 60);
    auction.type = type;

    // Validate auction parameters
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return false;
    }

    // Check if the duration is within namespace settings
    if (durationDays < namespaceInfo.minAuctionDuration ||
        durationDays > namespaceInfo.maxAuctionDuration) {
        return false;
    }

    // Check if reserve price meets minimum
    CAmount minPrice = CalculateMinPriceForUserID(name, namespaceId);
    if (reservePrice < minPrice) {
        return false;
    }

    // Create the auction
    return auctionManager->CreateAuction(auction, auctionId);
}

bool ReddIDManager::FinalizeUserIDAuction(const uint256& auctionId) {
    return auctionManager->FinalizeAuction(auctionId);
}

bool ReddIDManager::CancelUserIDAuction(const uint256& auctionId, const CKeyID& creator) {
    if (!m_initialized || !auctionManager) {
        LogPrintf("ERROR: ReddIDManager::CancelUserIDAuction: Manager not initialized\n");
        return false;
    }

    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        return false;
    }

    if (auction.creator != creator) {
        return false;
    }

    return auctionManager->CancelAuction(auctionId, creator);
}

bool ReddIDManager::TransferUserID(const std::string& name, const std::string& namespaceId,
                                 const CKeyID& from, const CKeyID& to) {
    UserIDInfo userID;
    if (!GetUserIDInfo(name, namespaceId, userID)) {
        return false;
    }

    if (userID.owner != from) {
        return false;
    }

    // Update owner
    userID.owner = to;

    // Save updated user ID
    return reddidDB->WriteUserID(name, namespaceId, userID);
}

bool ReddIDManager::RenewUserID(const std::string& name, const std::string& namespaceId, const CKeyID& owner) {
    UserIDInfo userID;

    if (!GetUserIDInfo(name, namespaceId, userID)) {
        return false;
    }

    if (userID.owner != owner) {
        return false;
    }

    // Get namespace info for renewal period
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return false;
    }

    // Update expiration time
    int64_t currentTime = GetTime();
    if (userID.expirationTime > currentTime) {
        // If not expired, add renewal period to current expiration
        userID.expirationTime += namespaceInfo.renewalPeriod * 24 * 60 * 60;
    } else {
        // If expired, set new expiration from current time
        userID.expirationTime = currentTime + namespaceInfo.renewalPeriod * 24 * 60 * 60;
    }

    // Save updated user ID
    return reddidDB->WriteUserID(name, namespaceId, userID);
}

bool ReddIDManager::ExpireUserID(const std::string& name, const std::string& namespaceId) {
    if (!reddidDB) return false;

    if (!reddidDB->EraseUserID(name, namespaceId)) {
        LogPrintf("ERROR: ReddIDManager::ExpireUserID: Failed to erase %s.%s\n", name, namespaceId);
        return false;
    }

    LogPrint(BCLog::REDDID, "Expired user ID: %s.%s\n", name, namespaceId);
    return true;
}

void ReddIDManager::CheckExpiredUserIDs() {
    if (!reddidDB) return;
    int64_t currentTime = GetTime();

    std::vector<std::string> namespaceIds;
    reddidDB->ListNamespaces(namespaceIds);

    int expiredCount = 0;
    for (const auto& nsId : namespaceIds) {
        // Get grace period from namespace config
        NamespaceInfo nsInfo;
        int gracePeriodSec = 30 * 24 * 60 * 60; // default 30 days
        if (namespaceManager && namespaceManager->GetNamespaceInfo(nsId, nsInfo)) {
            gracePeriodSec = nsInfo.gracePeriod * 24 * 60 * 60;
        }

        std::vector<std::string> names;
        reddidDB->ListUserIDs(nsId, names);

        for (const auto& name : names) {
            UserIDInfo info;
            if (reddidDB->ReadUserID(name, nsId, info)) {
                if (info.expirationTime > 0 &&
                    currentTime > info.expirationTime + gracePeriodSec) {
                    ExpireUserID(name, nsId);
                    expiredCount++;
                }
            }
        }
    }

    if (expiredCount > 0) {
        LogPrint(BCLog::REDDID, "Expired %d user IDs\n", expiredCount);
    }
}

bool ReddIDManager::CalculateReputation(const std::string& reddId, ReddIDReputation& result) {
    if (!m_initialized || !profileManager || !auctionManager) {
        LogPrintf("ERROR: ReddIDManager::CalculateReputation: Manager not initialized\n");
        return false;
    }

    // Get profile to resolve owner CKeyID for auction stats lookup
    ReddIDProfile profile;
    if (!profileManager->GetProfile(reddId, profile)) {
        return false;
    }

    // Gather auction/deposit stats for this user
    BidderStats bidderStats;
    auctionManager->GetBidderStats(profile.owner, bidderStats);

    // Delegate to ProfileManager with the auction stats
    return profileManager->CalculateReputation(reddId, bidderStats, result);
}

bool ReddIDManager::GetUserIDInfo(const std::string& name, const std::string& namespaceId, UserIDInfo& result) {
    return reddidDB->ReadUserID(name, namespaceId, result);
}

std::vector<BidInfo> ReddIDManager::GetAuctionBids(const uint256& auctionId) {
    return auctionManager->GetAuctionBids(auctionId);
}

CAmount ReddIDManager::CalculateMinPriceForNamespace(const std::string& namespaceId) {
    return namespaceManager->CalculateMinPrice(namespaceId);
}

CAmount ReddIDManager::CalculateMinPriceForUserID(const std::string& name, const std::string& namespaceId) {
    return namespaceManager->CalculateMinPriceForUserID(name, namespaceId);
}

CAmount ReddIDManager::CalculateRenewalFee(const std::string& name, const std::string& namespaceId) {
    // Get user ID info
    UserIDInfo userID;
    if (!GetUserIDInfo(name, namespaceId, userID)) {
        return 0;
    }

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return 0;
    }

    // Calculate equivalent auction price
    CAmount auctionPrice = CalculateMinPriceForUserID(name, namespaceId);

    // Calculate renewal price (default: 50% of auction price)
    CAmount renewalFee = auctionPrice * 0.5;

    // Apply usage discount if applicable
    if (userID.transactionCount > 0) {
        // Simple discount formula: 5% discount per 10 transactions, up to 25%
        double usageDiscount = std::min(0.25, (userID.transactionCount / 10) * 0.05);
        renewalFee = renewalFee * (1.0 - usageDiscount);
    }

    return renewalFee;
}
