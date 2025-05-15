// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/profile.h>
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
#include <string>
#include <vector>

ProfileManager::ProfileManager(ReddIDManager& manager)
    : m_initialized(false),
      m_running(false),
      reddIDManager(&manager),
      reddidDB(nullptr),
      reddidP2P(nullptr) {

    // Clear existing data
    profiles.clear();
    reputations.clear();
    connections.clear();
    namespaceResolution.clear();
}

ProfileManager::~ProfileManager() {
    Stop();
}

bool ProfileManager::Init(ReddIDManager* manager) {
  if (m_initialized) {
      LogPrint(BCLog::REDDID, "ProfileManager already initialized\n");
      return true;
  }

  if (!manager) {
      LogPrintf("ERROR: ProfileManager::Init: Null ReddIDManager\n");
      return false;
  }

  LogPrint(BCLog::REDDID, "Initializing Profile manager\n");

  try {
      // Store the manager reference
      reddIDManager = manager;

      reddidDB = manager->GetReddIDDB();
      if (!reddidDB) {
          LogPrint(BCLog::REDDID, "WARNING: ProfileManager::Init: reddidDB not available yet\n");
      }

      reddidP2P = manager->GetP2PManager();
      if (!reddidP2P) {
          LogPrint(BCLog::REDDID, "WARNING: ProfileManager::Init: reddidP2P not available yet\n");
      }

      // Load existing data from database
      if (!Load()) {
          LogPrintf("WARNING: ProfileManager::Init: Failed to load proifile data\n");
          // Continue anyway, database might be empty
      }

      m_initialized = true;
      LogPrint(BCLog::REDDID, "ProfileManager initialized successfully\n");
      return true;
  } catch (const std::exception& e) {
      LogPrintf("ERROR: ProfileManager::Init: Exception: %s\n", e.what());
      return false;
  }
}

bool ProfileManager::Start() {
    if (!m_initialized) {
        LogPrintf("ERROR: ProfileManager::Start: Not initialized\n");
        return false;
    }

    if (m_running) {
        LogPrint(BCLog::REDDID, "ProfileManager already running\n");
        return true;
    }

    LogPrint(BCLog::REDDID, "Starting ProfileManager\n");

    m_running = true;
    return true;
}

void ProfileManager::Interrupt() {
    if (!m_running) {
        return;
    }

    LogPrint(BCLog::REDDID, "Interrupting ProfileManager manager\n");
    m_running = false;
}

bool ProfileManager::Stop() {
    if (!m_initialized) {
        return true;
    }

    LogPrintf("Stopping ProfileManager\n");

    // Mark as not running
    m_running = false;

    // Save current state
    if (!Save()) {
        LogPrintf("WARNING: ProfileManager::Stop: Failed to save auction data\n");
    }

    // Clear references
    reddIDManager = nullptr;
    reddidDB = nullptr;
    reddidP2P = nullptr;

    m_initialized = false;

    LogPrint(BCLog::REDDID, "ProfileManager stopped\n");
    return true;
}

ReddIDDB* ProfileManager::GetDB() const {
    return reddidDB;
}

bool ProfileManager::Load() {
    LogPrintf("Loading profile data...\n");

    // Load profiles
    std::vector<std::string> reddIds;
    if (!reddidDB->ListProfiles(reddIds)) {
        LogPrintf("Error: Failed to list profiles\n");
        return false;
    }

    for (const auto& reddId : reddIds) {
        ReddIDProfile profile;
        if (reddidDB->ReadProfile(reddId, profile)) {
            profiles[reddId] = profile;

            // Load reputation
            ReddIDReputation reputation;
            if (reddidDB->ReadReputation(reddId, reputation)) {
                reputations[reddId] = reputation;
            }

            // Load connections
            std::vector<ReddIDConnection> profileConnections;
            reddidDB->ListConnections(reddId, profileConnections);
            connections[reddId] = profileConnections;

            // Load namespace resolutions
            std::map<std::string, std::string> resolutions;
            if (reddidDB->ListResolutions(reddId, resolutions)) {
                namespaceResolution[reddId] = resolutions;
            }
        }
    }

    LogPrintf("Loaded %d profiles\n", profiles.size());

    return true;
}

bool ProfileManager::Save() const {
    LogPrintf("Saving profile data...\n");

    // Save profiles
    for (const auto& pair : profiles) {
        if (!reddidDB->WriteProfile(pair.first, pair.second)) {
            LogPrintf("Error: Failed to save profile %s\n", pair.first);
            return false;
        }
    }

    // Save reputations
    for (const auto& pair : reputations) {
        if (!reddidDB->WriteReputation(pair.first, pair.second)) {
            LogPrintf("Error: Failed to save reputation for %s\n", pair.first);
            return false;
        }
    }

    // Save connections
    for (const auto& pair : connections) {
        for (const auto& connection : pair.second) {
            if (!reddidDB->WriteConnection(connection.fromReddId, connection.toReddId, connection)) {
                LogPrintf("Error: Failed to save connection from %s to %s\n",
                         connection.fromReddId, connection.toReddId);
                return false;
            }
        }
    }

    // Save namespace resolutions
    for (const auto& pair : namespaceResolution) {
        for (const auto& resolution : pair.second) {
            if (!reddidDB->WriteResolution(pair.first, resolution.first, resolution.second)) {
                LogPrintf("Error: Failed to save resolution for %s in namespace %s\n",
                         pair.first, resolution.first);
                return false;
            }
        }
    }

    return true;
}

bool ProfileManager::CreateProfile(const ReddIDProfile& profile) {
    // Check if profile already exists
    if (profiles.find(profile.reddId) != profiles.end()) {
        return false;
    }

    // Validate profile
    if (!ValidateProfile(profile)) {
        return false;
    }

    // Save profile to memory
    profiles[profile.reddId] = profile;

    // Initialize reputation
    ReddIDReputation rep;
    rep.reddId = profile.reddId;
    rep.overallScore = 50.0;  // Default starting score
    rep.longevityScore = 0.0;
    rep.transactionScore = 0.0;
    rep.engagementScore = 0.0;
    rep.verificationScore = 0.0;
    rep.auctionScore = 50.0;
    rep.lastCalculated = GetTime();

    reputations[profile.reddId] = rep;

    // Save to database
    if (!reddidDB->WriteProfile(profile.reddId, profile)) {
        profiles.erase(profile.reddId);
        return false;
    }

    if (!reddidDB->WriteReputation(profile.reddId, rep)) {
        return false;
    }

    return true;
}

bool ProfileManager::UpdateProfile(const ReddIDProfile& profile) {
    // Check if profile exists
    auto it = profiles.find(profile.reddId);
    bool isNew = (it == profiles.end());

    if (!isNew) {
        // Verify ownership
        if (it->second.owner != profile.owner) {
            return false;
        }
    }

    // Validate profile
    if (!ValidateProfile(profile)) {
        return false;
    }

    // If new profile, create it
    if (isNew) {
        return CreateProfile(profile);
    }

    // Update existing profile
    profiles[profile.reddId] = profile;

    // Update database
    if (!reddidDB->WriteProfile(profile.reddId, profile)) {
        return false;
    }

    // Announce via P2P
    reddidP2P->AnnounceProfileUpdate(profile.reddId, profile);

    return true;
}

bool ProfileManager::DeactivateProfile(const std::string& reddId, const CKeyID& owner) {
    // Check if profile exists
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return false;
    }

    // Verify ownership
    if (it->second.owner != owner) {
        return false;
    }

    // Deactivate profile
    it->second.active = false;
    it->second.lastUpdated = GetTime();

    // Update database
    return reddidDB->WriteProfile(reddId, it->second);
}

bool ProfileManager::ReactivateProfile(const std::string& reddId, const CKeyID& owner) {
    // Check if profile exists
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return false;
    }

    // Verify ownership
    if (it->second.owner != owner) {
        return false;
    }

    // Reactivate profile
    it->second.active = true;
    it->second.lastUpdated = GetTime();

    // Update database
    return reddidDB->WriteProfile(reddId, it->second);
}

bool ProfileManager::TransferProfile(const std::string& reddId, const CKeyID& from, const CKeyID& to) {
    // Check if profile exists
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return false;
    }

    // Verify current ownership
    if (it->second.owner != from) {
        return false;
    }

    // Transfer ownership
    it->second.owner = to;
    it->second.lastUpdated = GetTime();

    // Update database
    return reddidDB->WriteProfile(reddId, it->second);
}

bool ProfileManager::CreateConnection(const ReddIDConnection& connection) {
    // Check if source profile exists
    if (profiles.find(connection.fromReddId) == profiles.end()) {
        return false;
    }

    // Check if target profile exists
    if (profiles.find(connection.toReddId) == profiles.end()) {
        return false;
    }

    // Validate connection
    if (!ValidateConnection(connection)) {
        return false;
    }

    // Check for existing connection
    auto it = connections.find(connection.fromReddId);
    if (it != connections.end()) {
        for (auto& existingConn : it->second) {
            if (existingConn.toReddId == connection.toReddId) {
                // Update existing connection
                existingConn = connection;

                // Update database
                return reddidDB->WriteConnection(connection.fromReddId, connection.toReddId, connection);
            }
        }
    }

    // Add new connection
    connections[connection.fromReddId].push_back(connection);

    // Save to database
    return reddidDB->WriteConnection(connection.fromReddId, connection.toReddId, connection);
}

bool ProfileManager::UpdateConnection(const ReddIDConnection& connection) {
    // Check if source profile exists
    if (profiles.find(connection.fromReddId) == profiles.end()) {
        return false;
    }

    // Check if connection exists
    auto it = connections.find(connection.fromReddId);
    if (it == connections.end()) {
        return false;
    }

    bool found = false;
    for (auto& existingConn : it->second) {
        if (existingConn.toReddId == connection.toReddId) {
            // Verify ownership
            if (profiles[connection.fromReddId].owner != profiles[existingConn.fromReddId].owner) {
                return false;
            }

            // Update connection
            existingConn = connection;
            found = true;
            break;
        }
    }

    if (!found) {
        return false;
    }

    // Update database
    return reddidDB->WriteConnection(connection.fromReddId, connection.toReddId, connection);
}

bool ProfileManager::RemoveConnection(const std::string& fromReddId, const std::string& toReddId,
                                     const CKeyID& owner) {
    // Check if connection exists
    auto it = connections.find(fromReddId);
    if (it == connections.end()) {
        return false;
    }

    // Verify ownership
    if (profiles[fromReddId].owner != owner) {
        return false;
    }

    // Find connection
    auto connIt = std::find_if(it->second.begin(), it->second.end(),
                             [&toReddId](const ReddIDConnection& conn) {
                                 return conn.toReddId == toReddId;
                             });

    if (connIt == it->second.end()) {
        return false;
    }

    // Remove connection
    it->second.erase(connIt);

    // Remove from database
    return reddidDB->EraseConnection(fromReddId, toReddId);
}

bool ProfileManager::AddNamespaceResolution(const std::string& reddId, const std::string& namespaceId,
                                           const std::string& userId) {
    // Check if profile exists
    if (profiles.find(reddId) == profiles.end()) {
        return false;
    }

    // Add or update resolution
    namespaceResolution[reddId][namespaceId] = userId;

    // Save to database
    return reddidDB->WriteResolution(reddId, namespaceId, userId);
}

bool ProfileManager::UpdateNamespaceResolution(const std::string& reddId, const std::string& namespaceId,
                                              const std::string& userId) {
    // Check if profile exists
    if (profiles.find(reddId) == profiles.end()) {
        return false;
    }

    // Check if resolution exists
    auto it = namespaceResolution.find(reddId);
    if (it == namespaceResolution.end() || it->second.find(namespaceId) == it->second.end()) {
        return false;
    }

    // Update resolution
    namespaceResolution[reddId][namespaceId] = userId;

    // Save to database
    return reddidDB->WriteResolution(reddId, namespaceId, userId);
}

bool ProfileManager::RemoveNamespaceResolution(const std::string& reddId, const std::string& namespaceId) {
    // Check if resolution exists
    auto it = namespaceResolution.find(reddId);
    if (it == namespaceResolution.end() || it->second.find(namespaceId) == it->second.end()) {
        return false;
    }

    // Remove resolution
    it->second.erase(namespaceId);

    // Remove from database
    return reddidDB->EraseResolution(reddId, namespaceId);
}

bool ProfileManager::UpdateReputation(const ReddIDReputation& reputation) {
    // Check if profile exists
    if (profiles.find(reputation.reddId) == profiles.end()) {
        return false;
    }

    // Update reputation
    reputations[reputation.reddId] = reputation;

    // Save to database
    return reddidDB->WriteReputation(reputation.reddId, reputation);
}

bool ProfileManager::CalculateReputation(const std::string& reddId, ReddIDReputation& result) {
    // Check if profile exists
    auto profileIt = profiles.find(reddId);
    if (profileIt == profiles.end()) {
        return false;
    }

    // Get existing reputation if any
    auto it = reputations.find(reddId);
    if (it != reputations.end()) {
        result = it->second;
    } else {
        // Initialize new reputation
        result.reddId = reddId;
        result.overallScore = 50.0;
        result.longevityScore = 0.0;
        result.transactionScore = 0.0;
        result.engagementScore = 0.0;
        result.verificationScore = 0.0;
        result.auctionScore = 50.0;
    }

    // Calculate longevity score
    int64_t currentTime = GetTime();
    int64_t accountAge = currentTime - profileIt->second.creationTime;
    int64_t ageInDays = accountAge / (24 * 60 * 60);

    // Score increases with age, maxing out at 1 year (365 days)
    result.longevityScore = std::min(100.0, (ageInDays / 365.0) * 100.0);

    // Calculate verification score
    switch (profileIt->second.verificationStatus) {
        case VERIFICATION_NONE:
            result.verificationScore = 0.0;
            break;
        case VERIFICATION_SELF:
            result.verificationScore = 25.0;
            break;
        case VERIFICATION_COMMUNITY:
            result.verificationScore = 75.0;
            break;
        case VERIFICATION_OFFICIAL:
            result.verificationScore = 100.0;
            break;
    }

    // Calculate engagement score based on number of connections
    auto connIt = connections.find(reddId);
    if (connIt != connections.end()) {
        int connectionCount = connIt->second.size();
        // Score increases with connections, maxing out at 100
        result.engagementScore = std::min(100.0, connectionCount * 2.0);
    }

    // Calculate overall score (weighted average)
    result.overallScore = (
        result.longevityScore * 0.25 +
        result.transactionScore * 0.25 +
        result.engagementScore * 0.20 +
        result.verificationScore * 0.20 +
        result.auctionScore * 0.10);

    result.lastCalculated = currentTime;

    // Update stored reputation
    reputations[reddId] = result;

    // Save to database
    reddidDB->WriteReputation(reddId, result);

    return true;
}

bool ProfileManager::IsProfileOwner(const std::string& reddId, const CKeyID& keyId) const {
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return false;
    }

    return it->second.owner == keyId;
}

bool ProfileManager::IsProfileActive(const std::string& reddId) const {
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return false;
    }

    return it->second.active;
}

bool ProfileManager::ProfileExists(const std::string& reddId) const {
    return profiles.find(reddId) != profiles.end() || reddidDB->ExistsProfile(reddId);
}

bool ProfileManager::IsValidProfileName(const std::string& reddId) const {
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

bool ProfileManager::GetProfile(const std::string& reddId, ReddIDProfile& result) const {
    auto it = profiles.find(reddId);
    if (it == profiles.end()) {
        return reddidDB->ReadProfile(reddId, result);
    }

    result = it->second;
    return true;
}

bool ProfileManager::GetReputation(const std::string& reddId, ReddIDReputation& result) const {
    auto it = reputations.find(reddId);
    if (it == reputations.end()) {
        return reddidDB->ReadReputation(reddId, result);
    }

    result = it->second;
    return true;
}

bool ProfileManager::GetConnection(const std::string& fromReddId, const std::string& toReddId,
                                  ReddIDConnection& result) const {
    // First check connections in memory
    auto it = connections.find(fromReddId);
    if (it != connections.end()) {
        for (const auto& connection : it->second) {
            if (connection.toReddId == toReddId) {
                result = connection;
                return true;
            }
        }
    }

    // If not found in memory, check the database
    return reddidDB->ReadConnection(fromReddId, toReddId, result);
}

std::vector<ReddIDConnection> ProfileManager::GetConnections(const std::string& reddId) const {
    auto it = connections.find(reddId);
    if (it == connections.end()) {
        std::vector<ReddIDConnection> result;
        reddidDB->ListConnections(reddId, result);
        return result;
    }

    return it->second;
}

std::string ProfileManager::ResolveNamespace(const std::string& reddId, const std::string& namespaceId) const {
    auto it = namespaceResolution.find(reddId);
    if (it == namespaceResolution.end() || it->second.find(namespaceId) == it->second.end()) {
        std::string result;
        reddidDB->ReadResolution(reddId, namespaceId, result);
        return result;
    }

    return it->second.at(namespaceId);
}

std::string ProfileManager::ResolveReddID(const std::string& userId, const std::string& namespaceId) const {
    std::string result;
    reddidDB->ReadResolutionByUserID(userId, namespaceId, result);
    return result;
}

std::vector<ReddIDProfile> ProfileManager::GetProfiles(int offset, int limit) const {
    std::vector<ReddIDProfile> result;

    // Convert map to vector
    std::vector<ReddIDProfile> allProfiles;
    for (const auto& pair : profiles) {
        allProfiles.push_back(pair.second);
    }

    // Apply pagination
    int end = std::min(offset + limit, static_cast<int>(allProfiles.size()));
    for (int i = offset; i < end; i++) {
        result.push_back(allProfiles[i]);
    }

    return result;
}

bool ProfileManager::GetProfilesByOwner(const CKeyID& owner, std::vector<std::string>& reddIds) {
    // First check in-memory cache
    for (const auto& pair : profiles) {
        if (pair.second.owner == owner) {
            reddIds.push_back(pair.first);
        }
    }

    // If nothing found in memory, check database
    if (reddIds.empty()) {
        return reddidDB->ListProfilesByOwner(owner, reddIds);
    }

    return true;
}

bool ProfileManager::ProcessTransaction(const CTransaction& tx, int nHeight) {
    // Process ReddID profile-related transactions
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
                    if (opCode == OP_REDDID_AUCTION_CREATE) {
                        // Parse ReddID auction creation
                        LogPrintf("Received ReddID auction creation transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    } else if (opCode == OP_REDDID_AUCTION_BID) {
                        // Parse ReddID auction bid
                        LogPrintf("Received ReddID auction bid transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    } else if (opCode == OP_REDDID_AUCTION_FINALIZE) {
                        // Parse ReddID auction finalization
                        LogPrintf("Received ReddID auction finalization transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    } else if (opCode == OP_REDDID_PROFILE_UPDATE) {
                        // Parse profile update
                        LogPrintf("Received ReddID profile update transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    } else if (opCode == OP_REDDID_CONNECTION) {
                        // Parse connection operation
                        LogPrintf("Received ReddID connection transaction: %s\n", tx.GetHash().ToString());
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool ProfileManager::ValidateProfile(const ReddIDProfile& profile) {
    // Check ReddID format
    if (!IsValidProfileName(profile.reddId)) {
        return false;
    }

    // Check display name length
    if (profile.displayName.size() > 64) {
        return false;
    }

    // Check bio length
    if (profile.bio.size() > 256) {
        return false;
    }

    // Check verification status
    if (profile.verificationStatus < VERIFICATION_NONE ||
        profile.verificationStatus > VERIFICATION_OFFICIAL) {
        return false;
    }

    return true;
}

bool ProfileManager::ValidateConnection(const ReddIDConnection& connection) {
    // Check that it's not a self-connection
    if (connection.fromReddId == connection.toReddId) {
        return false;
    }

    // Check valid connection type
    if (connection.connectionType < CONNECTION_FOLLOW ||
        connection.connectionType > CONNECTION_BLOCK) {
        return false;
    }

    // Check valid visibility
    if (connection.visibility < 0 || connection.visibility > 2) {
        return false;
    }

    return true;
}

uint256 ProfileManager::CalculateProfileHash(const ReddIDProfile& profile) {
    // Serialize profile to calculate hash
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << profile;

    return Hash(Span<const unsigned char>(reinterpret_cast<const unsigned char*>(ss.data()), ss.size()));
}

bool ProfileManager::UpdateReputationScore(const std::string& reddId) {
    ReddIDReputation reputation;
    return CalculateReputation(reddId, reputation);
}
