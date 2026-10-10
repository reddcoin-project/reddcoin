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
#include <util/system.h>
#include <validation.h>

#include <exception>
#include <memory>
#include <string>

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

CAmount ReddIDManager::CalculateMinPriceForNamespace(const std::string& namespaceId) {
    return namespaceManager->CalculateMinPrice(namespaceId);
}

CAmount ReddIDManager::CalculateMinPriceForUserID(const std::string& name, const std::string& namespaceId) {
    return namespaceManager->CalculateMinPriceForUserID(name, namespaceId);
}
