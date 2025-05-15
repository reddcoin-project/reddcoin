// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/reddid.h>
#include <id/namespace.h>
#include <id/auction.h>
#include <id/profile.h>
#include <id/reddid_db.h>
#include <id/reddid_p2p.h>

#include <core_io.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <validation.h>
#include <validationinterface.h>
#include <scheduler.h>
#include <script/script.h>
#include <script/standard.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <consensus/validation.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
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
    bool enableReddID = gArgs.GetBoolArg("-reddid", true);
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
        p2pManager = std::make_unique<ReddIDP2PManager>(*node);

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

        if (!p2pManager->Init(this)) {
            LogPrintf("Failed to initialize ReddID P2P manager\n");
            return false;
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

    // Register with validation interface
    RegisterValidationInterface(this);

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

    // Start P2P manager
    if (!p2pManager->Start()) {
        LogPrintf("Error: Failed to start ReddID P2P manager\n");
        return false;
    }

    // Register message handlers
    if (!p2pManager->RegisterMessageHandlers()) {
        LogPrintf("Warning: Failed to register ReddID message handlers\n");
        // Continue anyway, as some functionality may still work
    }

    // Schedule periodic tasks
    if (node && node->scheduler) {
        node->scheduler->scheduleEvery([this]() {
            // Check for expired auctions
            auctionManager->CheckExpiredAuctions();
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

    if (p2pManager) {
        p2pManager->Interrupt();
    }

    // Flag as not running, but don't clean up yet
    m_running = false;
}

bool ReddIDManager::Stop() {
    if (!m_running) {
        return true;
    }

    LogPrintf("Stopping ReddID manager\n");

    // Unregister from validation interface
    UnregisterValidationInterface(this);

    // Stop P2P manager
    if (p2pManager) {
        p2pManager->Stop();
    }

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
    p2pManager.reset();
    profileManager.reset();
    auctionManager.reset();
    namespaceManager.reset();
    reddidDB.reset();

    m_running = false;
    m_initialized = false;

    return true;
}

// ValidationInterface implementations
void ReddIDManager::TransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) {
    if (m_running) {
        ProcessTransaction(*tx, 0);  // Height 0 for mempool transactions
    }
}

void ReddIDManager::BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) {
    if (!m_running) return;

    // Process transactions in the block
    for (const auto& tx : block->vtx) {
        ProcessTransaction(*tx, pindex->nHeight);
    }
}

void ReddIDManager::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) {
    // Handle block disconnection if needed
    // For now, we'll just log that a block was disconnected
    LogPrintf("ReddID: Block disconnected at height %d, hash %s\n",
             pindex->nHeight, pindex->GetBlockHash().ToString());
}

bool ReddIDManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    // Ignore coinbase transactions
    if (tx.IsCoinBase()) {
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

bool ReddIDManager::ParseReddIDOpReturn(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data) {
    // Check if this is an OP_RETURN script
    if (script.size() < 2 || script[0] != OP_RETURN) {
        return false;
    }

    // Extract the data from OP_RETURN
    std::vector<unsigned char> vchData;
    CScript::const_iterator pc = script.begin() + 1;
    opcodetype opcodeRet;
    if (!script.GetOp(pc, opcodeRet, vchData) || vchData.size() < 2) {
        return false;
    }

    // Check if this is a ReddID operation (first byte should be 'R' and second byte an op code)
    if (vchData.size() < 2 || vchData[0] != 'R') {
        return false;
    }

    // Extract operation code and data
    opCode = vchData[1];
    data = std::vector<unsigned char>(vchData.begin() + 2, vchData.end());

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
    if (name.size() < namespaceInfo.minLength || name.size() > namespaceInfo.maxLength) {
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

bool ReddIDManager::CreateNamespaceAuction(const std::string& namespaceId, const CKeyID& creator, 
                                         CAmount reservePrice, int durationDays, AuctionType type,
                                         uint256& auctionId) {
    if (!m_initialized || !namespaceManager) {
        LogPrintf("ERROR: ReddIDManager::CreateNamespaceAuction: Manager not initialized\n");
        return false;
    }

    return namespaceManager->CreateNamespaceAuction(namespaceId, creator, reservePrice,
                                                   durationDays, type, auctionId);
}

bool ReddIDManager::BidOnNamespaceAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) {
    if (!m_initialized || !namespaceManager) {
        LogPrintf("ERROR: ReddIDManager::BidOnNamespaceAuction: Manager not initialized\n");
        return false;
    }

    uint256 bidId;
    return namespaceManager->BidOnNamespaceAuction(auctionId, bidder, bidAmount, bidId);
}

bool ReddIDManager::FinalizeNamespaceAuction(const uint256& auctionId) {
    if (!m_initialized || !namespaceManager) {
        LogPrintf("ERROR: ReddIDManager::FinalizeNamespaceAuction: Manager not initialized\n");
        return false;
    }

    return namespaceManager->FinalizeNamespaceAuction(auctionId);
}

bool ReddIDManager::CancelNamespaceAuction(const uint256& auctionId, const CKeyID& creator) {
    if (!m_initialized || !namespaceManager) {
        LogPrintf("ERROR: ReddIDManager::CancelNamespaceAuction: Manager not initialized\n");
        return false;
    }

    return namespaceManager->CancelNamespaceAuction(auctionId, creator);
}

bool ReddIDManager::UpdateNamespaceConfig(const std::string& namespaceId, const NamespaceInfo& config, const CKeyID& owner) {

    if (!m_initialized || !namespaceManager) {
        LogPrintf("ERROR: ReddIDManager::UpdateNamespaceConfig: Manager not initialized\n");
        return false;
    }

    NamespaceInfo existingConfig;

    if (!namespaceManager->GetNamespaceInfo(namespaceId, existingConfig)) {
        return false;
    }

    if (existingConfig.owner != owner) {
        return false;
    }

    // Only update configuration if namespace exists and owner matches
    return namespaceManager->UpdateNamespace(config);
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

bool ReddIDManager::BidOnUserIDAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) {

    if (!m_initialized || !auctionManager) {
      LogPrintf("ERROR: ReddIDManager::BidOnUserIDAuction: Manager not initialized\n");
      return false;
    }

    uint256 bidId;

    return auctionManager->PlaceBid(auctionId, bidder, bidAmount, bidId);
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

bool ReddIDManager::UpdateProfile(const std::string& reddId, const ReddIDProfile& profile, const CKeyID& owner) {
    ReddIDProfile existingProfile;

    if (profileManager->GetProfile(reddId, existingProfile)) {
        // If profile exists, check if owner matches
        if (existingProfile.owner != owner) {
            return false;
        }
    }

    // Create or update the profile
    return profileManager->UpdateProfile(profile);
}

bool ReddIDManager::CreateConnection(const std::string& fromReddId, const std::string& toReddId, 
                                   SocialConnectionType type, int visibility, const CKeyID& owner) {

    ReddIDProfile fromProfile;
    if (!profileManager->GetProfile(fromReddId, fromProfile)) {
        return false;
    }

    // Verify ownership
    if (fromProfile.owner != owner) {
        return false;
    }

    // Check if target profile exists
    ReddIDProfile toProfile;
    if (!profileManager->GetProfile(toReddId, toProfile)) {
        return false;
    }

    // Create connection
    ReddIDConnection connection;
    connection.fromReddId = fromReddId;
    connection.toReddId = toReddId;
    connection.connectionType = type;
    connection.creationTime = GetTime();
    connection.lastInteraction = connection.creationTime;
    connection.visibility = visibility;

    return profileManager->CreateConnection(connection);
}

bool ReddIDManager::UpdateReputation(const std::string& reddId, const ReddIDReputation& reputation) {
    return profileManager->UpdateReputation(reputation);
}

bool ReddIDManager::GetNamespaceInfo(const std::string& namespaceId, NamespaceInfo& result) {
    return namespaceManager->GetNamespaceInfo(namespaceId, result);
}

bool ReddIDManager::GetUserIDInfo(const std::string& name, const std::string& namespaceId, UserIDInfo& result) {
    return reddidDB->ReadUserID(name, namespaceId, result);
}

bool ReddIDManager::GetAuctionInfo(const uint256& auctionId, AuctionInfo& result) {
    return auctionManager->GetAuctionInfo(auctionId, result);
}

bool ReddIDManager::GetProfile(const std::string& reddId, ReddIDProfile& result) {
    return profileManager->GetProfile(reddId, result);
}

bool ReddIDManager::GetReputation(const std::string& reddId, ReddIDReputation& result) {
    return profileManager->GetReputation(reddId, result);
}

std::vector<AuctionInfo> ReddIDManager::GetActiveNamespaceAuctions() {
    return namespaceManager->GetActiveAuctions();
}

std::vector<AuctionInfo> ReddIDManager::GetActiveUserIDAuctions(const std::string& namespaceId) {
    if (namespaceId.empty()) {
        return auctionManager->GetActiveAuctions();
    } else {
        return auctionManager->GetAuctionsByNamespace(namespaceId);
    }
}

std::vector<BidInfo> ReddIDManager::GetAuctionBids(const uint256& auctionId) {
    return auctionManager->GetAuctionBids(auctionId);
}

CAmount ReddIDManager::CalculateMinPriceForNamespace(const std::string& namespaceId) {
    return namespaceManager->CalculateMinPrice(namespaceId);
}

CAmount ReddIDManager::CalculateMinPriceForUserID(const std::string& name, const std::string& namespaceId) {
    CAmount basePrice = REDDID_MIN_FEE;

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        return basePrice;
    }

    // Get pricing tiers for this namespace
    std::vector<PricingTier> tiers = namespaceManager->GetPricingTiers(namespaceId);

    // Find the appropriate tier based on name length
    CAmount tierPrice = basePrice;
    for (const auto& tier : tiers) {
        if (name.size() >= tier.minLength) {
            tierPrice = tier.minPrice;
        }
    }

    return tierPrice;
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

CScript ReddIDManager::CreateNamespaceAuctionOpReturn(const std::string& namespaceId, const uint256& auctionId) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_NAMESPACE_AUCTION_CREATE);  // Operation code

    // Add namespace ID (up to 15 chars)
    std::vector<unsigned char> nsId(namespaceId.begin(), namespaceId.end());
    data.insert(data.end(), nsId.begin(), nsId.end());

    // Add auction ID
    std::vector<unsigned char> aucId(auctionId.begin(), auctionId.end());
    data.insert(data.end(), aucId.begin(), aucId.end());

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}

CScript ReddIDManager::CreateNamespaceBidOpReturn(const uint256& auctionId, CAmount bidAmount) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_NAMESPACE_AUCTION_BID);  // Operation code

    // Add auction ID
    std::vector<unsigned char> aucId(auctionId.begin(), auctionId.end());
    data.insert(data.end(), aucId.begin(), aucId.end());

    // Add bid amount
    std::vector<unsigned char> amountData(8);
    WriteLE64(amountData.data(), bidAmount);
    data.insert(data.end(), amountData.begin(), amountData.end());

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}

CScript ReddIDManager::CreateUserIDAuctionOpReturn(const std::string& name, const std::string& namespaceId,
                                                const uint256& auctionId) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_AUCTION_CREATE);  // Operation code

    // Add name
    std::vector<unsigned char> nameData(name.begin(), name.end());
    data.insert(data.end(), nameData.begin(), nameData.end());

    // Add separator
    data.push_back('.');

    // Add namespace ID
    std::vector<unsigned char> nsId(namespaceId.begin(), namespaceId.end());
    data.insert(data.end(), nsId.begin(), nsId.end());

    // Add auction ID
    std::vector<unsigned char> aucId(auctionId.begin(), auctionId.end());
    data.insert(data.end(), aucId.begin(), aucId.end());

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}

CScript ReddIDManager::CreateUserIDBidOpReturn(const uint256& auctionId, CAmount bidAmount) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_AUCTION_BID);  // Operation code

    // Add auction ID
    std::vector<unsigned char> aucId(auctionId.begin(), auctionId.end());
    data.insert(data.end(), aucId.begin(), aucId.end());

    // Add bid amount
    std::vector<unsigned char> amountData(8);
    WriteLE64(amountData.data(), bidAmount);
    data.insert(data.end(), amountData.begin(), amountData.end());

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}

CScript ReddIDManager::CreateProfileUpdateOpReturn(const std::string& reddId, const uint256& profileHash) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_REDDID_PROFILE_UPDATE);  // Operation code

    // Add ReddID
    std::vector<unsigned char> reddIdData(reddId.begin(), reddId.end());
    data.insert(data.end(), reddIdData.begin(), reddIdData.end());

    // Add profile hash
    std::vector<unsigned char> hashData(profileHash.begin(), profileHash.end());
    data.insert(data.end(), hashData.begin(), hashData.end());

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}

CScript ReddIDManager::CreateConnectionOpReturn(const std::string& fromReddId, const std::string& toReddId,
                                             SocialConnectionType type) {
    std::vector<unsigned char> data;
    data.push_back('R');  // ReddID prefix
    data.push_back(OP_REDDID_CONNECTION);  // Operation code

    // Add source ReddID
    std::vector<unsigned char> fromData(fromReddId.begin(), fromReddId.end());
    data.insert(data.end(), fromData.begin(), fromData.end());

    // Add separator
    data.push_back(':');

    // Add target ReddID
    std::vector<unsigned char> toData(toReddId.begin(), toReddId.end());
    data.insert(data.end(), toData.begin(), toData.end());

    // Add connection type
    data.push_back(static_cast<unsigned char>(type));

    // Create OP_RETURN script
    CScript script;
    script << OP_RETURN << data;

    return script;
}
