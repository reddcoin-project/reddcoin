// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_NAMESPACE_H
#define BITCOIN_ID_NAMESPACE_H

#include <id/reddid.h>

#include <amount.h>
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
 * Class to manage namespace operations in the ReddID system
 */
class NamespaceManager {
private:
    std::map<std::string, NamespaceInfo> namespaces;
    std::map<uint256, AuctionInfo> namespaceAuctions;
    std::map<uint256, std::vector<BidInfo>> namespaceAuctionBids;
    std::map<std::string, std::vector<PricingTier>> namespacePricingTiers;
    
    ReddIDManager* reddIDManager;        // Reference to the manager
    ReddIDDB* reddidDB;                  // Pointer to the database
    ReddIDP2PManager* reddidP2P;         // Pointer to the P2P manager

    // Private helper methods
    bool ValidateNamespaceConfig(const NamespaceInfo& config);
    bool ValidateNamespaceCharacters(const std::string& namespaceId);
    uint256 CalculateConfigHash(const NamespaceInfo& config);
    void DistributeNamespaceAuctionProceeds(const AuctionInfo& auction, const BidInfo& winningBid);
    
public:
    NamespaceManager(ReddIDManager& manager);
    ~NamespaceManager();
    
    // Core namespace operations
    bool CreateNamespace(const NamespaceInfo& namespaceInfo);
    bool UpdateNamespace(const NamespaceInfo& namespaceInfo);
    bool RenewNamespace(const std::string& namespaceId, const CKeyID& owner, int renewalDays);
    bool TransferNamespace(const std::string& namespaceId, const CKeyID& from, const CKeyID& to);
    bool ExpireNamespace(const std::string& namespaceId);
    
    // Namespace auction operations
    bool CreateNamespaceAuction(const std::string& namespaceId, const CKeyID& creator,
                              CAmount reservePrice, int durationDays, AuctionType type,
                              uint256& auctionId);
    bool BidOnNamespaceAuction(const uint256& auctionId, const CKeyID& bidder, 
                             CAmount bidAmount, uint256& bidId);
    bool FinalizeNamespaceAuction(const uint256& auctionId);
    bool CancelNamespaceAuction(const uint256& auctionId, const CKeyID& creator);
    
    // Namespace price tier operations
    bool AddPricingTier(const std::string& namespaceId, int minLength, CAmount minPrice);
    bool UpdatePricingTier(const std::string& namespaceId, int minLength, CAmount minPrice);
    bool RemovePricingTier(const std::string& namespaceId, int minLength);
    
    // Validation methods
    bool IsNamespaceAvailable(const std::string& namespaceId) const;
    bool IsNamespaceValid(const std::string& namespaceId) const;
    bool IsOwner(const std::string& namespaceId, const CKeyID& keyId) const;
    bool HasExpired(const std::string& namespaceId) const;
    
    // Query methods
    bool GetNamespaceInfo(const std::string& namespaceId, NamespaceInfo& result) const;
    bool GetAuctionInfo(const uint256& auctionId, AuctionInfo& result) const;
    std::vector<AuctionInfo> GetActiveAuctions() const;
    std::vector<BidInfo> GetAuctionBids(const uint256& auctionId) const;
    std::vector<NamespaceInfo> GetNamespaces() const;
    std::vector<PricingTier> GetPricingTiers(const std::string& namespaceId) const;
    CAmount CalculateMinPrice(const std::string& namespaceId) const;
    
    // Configuration validation
    bool ValidateNamespaceID(const std::string& name) const;
    
    // Fee calculations
    CAmount CalculateRenewalFee(const std::string& namespaceId) const;
    
    // Transaction processing
    bool ProcessTransaction(const CTransaction& tx, int nHeight);
    
    // Database operations
    bool Load();
    bool Save() const;
    
    // Access to the database and other components through ReddIDManager
    ReddIDDB* GetDB() const;
    ReddIDP2PManager* GetP2P() const;
};

#endif // BITCOIN_ID_NAMESPACE_H
