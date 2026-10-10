// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_AUCTION_H
#define BITCOIN_ID_AUCTION_H

#include <id/reddid.h>

#include <amount.h>
#include <key.h>
#include <primitives/transaction.h>
#include <threadsafety.h>
#include <uint256.h>

#include <atomic>
#include <cstdint>
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
    std::map<COutPoint, DepositLock> depositLocks;
    std::map<std::string, AuctionConfig> auctionConfigs;

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
    bool UndoTransaction(const CTransaction& tx, int nHeight);
    // Reads the UTXO set through CoinsTip() without holding cs_main. Exempt
    // from the thread safety analysis until the locking is addressed.
    bool GetTransactionSender(const CTransaction& tx, CKeyID& sender) NO_THREAD_SAFETY_ANALYSIS;

    // Database operations
    bool Load();
    bool Save() const;

    // Deposit lock operations
    bool IsDepositLocked(const COutPoint& utxo) const;
    bool GetDepositLock(const COutPoint& utxo, DepositLock& lock) const;
    bool CreateDepositLock(const COutPoint& utxo, const uint256& auctionId, const uint256& bidId,
                          const CKeyID& bidder, CAmount amount, int lockHeight);
    bool UpdateDepositLock(const COutPoint& utxo, DepositState newState);
    bool ReleaseLosingDeposits(const uint256& auctionId, const uint256& winningBidId);
    std::vector<DepositLock> GetDepositLocksForAuction(const uint256& auctionId) const;

    // Auction config operations
    bool GetAuctionConfig(const std::string& configId, AuctionConfig& config) const;
    bool SetAuctionConfig(const AuctionConfig& config);

    // Settlement operations
    bool BeginSettlement(const uint256& auctionId, const CTransaction& tx);
    bool CompleteSettlement(const uint256& auctionId, const uint256& bidId, const CTransaction& tx);
    bool ProcessDefault(const uint256& auctionId, const uint256& defaultedBidId);
    bool CreateReAuction(const AuctionInfo& originalAuction, uint256& newAuctionId);

    // UTXO lock enforcement
    bool CheckDepositLocks(const CTransaction& tx) const;
    bool IsValidSettlementTx(const CTransaction& tx, const COutPoint& lockedUtxo) const;
    bool IsValidForfeitTx(const CTransaction& tx, const COutPoint& lockedUtxo) const;

    // Reputation statistics for a bidder
    bool GetBidderStats(const CKeyID& bidder, BidderStats& stats) const;

    // Settlement deadline monitoring
    void CheckSettlementDeadlines(int currentHeight);

    // Periodic tasks
    void CheckExpiredAuctions();

    // Access to the database and other components
    ReddIDDB* GetDB() const { return reddidDB; }
    ReddIDP2PManager* GetP2P() const { return reddidP2P; }
    NamespaceManager* GetNamespaceManager() const { return namespaceManager; }

    void SetChainState(CChainState* cs) { chainstate = cs; }
};

#endif // BITCOIN_ID_AUCTION_H
