// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_AUCTION_H
#define BITCOIN_ID_AUCTION_H

#include <id/reddid.h>

#include <amount.h>
#include <key.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <map>
#include <set>
#include <string>
#include <vector>

class CChainState;
class ReddIDManager;
class ReddIDDB;
class ReddIDP2PManager;
class NamespaceManager;

/**
 * Class to manage auction operations for both namespaces and user IDs
 */
class AuctionManager {
 private:
    std::map<uint256, AuctionInfo> auctions;
    std::map<uint256, std::vector<BidInfo>> auctionBids;
    std::set<uint256> activeAuctions;

    CChainState* chainstate;  // Reference to the chain state

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};

    // References to other components
    ReddIDManager* reddIDManager;        // Reference to the manager
    ReddIDDB* reddidDB;                  // Pointer to the database
    ReddIDP2PManager* reddidP2P;         // Pointer to the P2P manager
    NamespaceManager* namespaceManager;  // Pointer to the Namespace Man

    // Private helper methods
    bool ValidateAuctionParameters(const AuctionInfo& auction);
    bool ValidateBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount);
    uint256 CalculateAuctionId(const std::string& name, const std::string& namespaceId, int64_t startTime);
    uint256 CalculateBidId(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount, int64_t bidTime);
    void DistributeAuctionProceeds(const AuctionInfo& auction, const BidInfo& winningBid);
    bool RefundLosingBids(const uint256& auctionId, const uint256& winningBidId);
    bool UpdateAuctionState(uint256& auctionId, AuctionState newState);
    bool ExtendAuctionIfNeeded(AuctionInfo& auction, int64_t bidTime);

 public:
    AuctionManager(ReddIDManager& manager);
    ~AuctionManager();

    // Lifecycle methods
    bool Init(ReddIDManager* manager);
    bool Start();
    void Interrupt();
    bool Stop();
    bool IsInitialized() const { return m_initialized; }
    bool IsRunning() const { return m_running; }

    // Core auction operations
    bool CreateAuction(AuctionInfo& auction, uint256& auctionId);
    bool PlaceBid(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount,
                uint256& bidId, int64_t bidTime = 0);
    bool FinalizeAuction(const uint256& auctionId);
    bool CancelAuction(const uint256& auctionId, const CKeyID& creator);
    bool ProcessExpiredAuctions(int64_t currentTime);

    // Validation methods
    bool IsAuctionActive(const uint256& auctionId) const;
    bool IsAuctionEnded(const uint256& auctionId) const;
    bool IsAuctionCreator(const uint256& auctionId, const CKeyID& keyId) const;
    bool IsHighestBidder(const uint256& auctionId, const CKeyID& keyId) const;

    // Query methods
    bool GetAuctionInfo(const uint256& auctionId, AuctionInfo& result) const;
    bool GetBidInfo(const uint256& bidId, BidInfo& result) const;
    std::vector<AuctionInfo> GetActiveAuctions() const;
    std::vector<AuctionInfo> GetAuctionsByNamespace(const std::string& namespaceId) const;
    std::vector<BidInfo> GetAuctionBids(const uint256& auctionId) const;
    BidInfo GetHighestBid(const uint256& auctionId) const;

    // Transaction processing
    bool ProcessTransaction(const CTransaction& tx, int nHeight);
    bool GetTransactionSender(const CTransaction& tx, CKeyID& sender);
    bool VerifyTransactionOwnership(const CTransaction& tx, const CKeyID& expectedOwner);


    // Database operations
    bool Load();
    bool Save() const;

    // Periodic tasks
    void CheckExpiredAuctions();

    // Access to the database and other components
    ReddIDDB* GetDB() const { return reddidDB; }
    ReddIDP2PManager* GetP2P() const { return reddidP2P; }
    NamespaceManager* GetNamespaceManager() const { return namespaceManager; }

    void SetChainState(CChainState* cs) { chainstate = cs; }
};

#endif  // BITCOIN_ID_AUCTION_H
