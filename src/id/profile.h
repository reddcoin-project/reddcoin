// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_PROFILE_H
#define BITCOIN_ID_PROFILE_H

#include <id/reddid.h>

#include <key.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <map>
#include <set>
#include <string>
#include <vector>

class ReddIDManager;
class ReddIDDB;
class ReddIDP2PManager;

/**
 * Class to manage ReddID profiles and social connections
 */
class ProfileManager {
 private:
    std::map<std::string, ReddIDProfile> profiles;
    std::map<std::string, ReddIDReputation> reputations;
    std::map<std::string, std::vector<ReddIDConnection>> connections;
    std::map<std::string, std::map<std::string, std::string>> namespaceResolution;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};

    ReddIDManager* reddIDManager;  // Reference to the manager
    ReddIDDB* reddidDB;  // Pointer to the database
    ReddIDP2PManager* reddidP2P;  // Pointer to the P2P manager

    // Private helper methods
    bool ValidateProfile(const ReddIDProfile& profile);
    bool ValidateConnection(const ReddIDConnection& connection);
    uint256 CalculateProfileHash(const ReddIDProfile& profile);
    bool UpdateReputationScore(const std::string& reddId);

 public:
    ProfileManager(ReddIDManager& manager);
    ~ProfileManager();

    // Lifecycle methods
    bool Init(ReddIDManager* manager);
    bool Start();
    void Interrupt();
    bool Stop();
    bool IsInitialized() const { return m_initialized; }
    bool IsRunning() const { return m_running; }


    // Core profile operations
    bool CreateProfile(const ReddIDProfile& profile);
    bool UpdateProfile(const ReddIDProfile& profile);
    bool DeactivateProfile(const std::string& reddId, const CKeyID& owner);
    bool ReactivateProfile(const std::string& reddId, const CKeyID& owner);
    bool TransferProfile(const std::string& reddId, const CKeyID& from, const CKeyID& to);

    // Social connection operations
    bool CreateConnection(const ReddIDConnection& connection);
    bool UpdateConnection(const ReddIDConnection& connection);
    bool RemoveConnection(const std::string& fromReddId, const std::string& toReddId,
                        const CKeyID& owner);

    // Namespace resolution operations
    bool AddNamespaceResolution(const std::string& reddId, const std::string& namespaceId,
                              const std::string& userId);
    bool UpdateNamespaceResolution(const std::string& reddId, const std::string& namespaceId,
                                 const std::string& userId);
    bool RemoveNamespaceResolution(const std::string& reddId, const std::string& namespaceId);

    // Reputation operations
    bool UpdateReputation(const ReddIDReputation& reputation);
    bool CalculateReputation(const std::string& reddId, ReddIDReputation& result);

    // Validation methods
    bool IsProfileOwner(const std::string& reddId, const CKeyID& keyId) const;
    bool IsProfileActive(const std::string& reddId) const;
    bool ProfileExists(const std::string& reddId) const;
    bool IsValidProfileName(const std::string& reddId) const;

    // Query methods
    bool GetProfile(const std::string& reddId, ReddIDProfile& result) const;
    bool GetReputation(const std::string& reddId, ReddIDReputation& result) const;
    bool GetConnection(const std::string& fromReddId, const std::string& toReddId, ReddIDConnection& result) const;
    std::vector<ReddIDConnection> GetConnections(const std::string& reddId) const;
    std::string ResolveNamespace(const std::string& reddId, const std::string& namespaceId) const;
    std::string ResolveReddID(const std::string& userId, const std::string& namespaceId) const;
    std::vector<ReddIDProfile> GetProfiles(int offset = 0, int limit = 100) const;
    bool GetProfilesByOwner(const CKeyID& owner, std::vector<std::string>& reddIds);

    // Transaction processing
    bool ProcessTransaction(const CTransaction& tx, int nHeight);

    // Database operations
    bool Load();
    bool Save() const;

    // Access to the database and other components through ReddIDManager
    ReddIDDB* GetDB() const;
    ReddIDP2PManager* GetP2P() const;
};

#endif  // BITCOIN_ID_PROFILE_H
