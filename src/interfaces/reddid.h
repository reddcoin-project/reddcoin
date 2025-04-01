// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_REDDID_H
#define BITCOIN_INTERFACES_REDDID_H

#include <amount.h>
#include <id/reddid.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CKeyID;
class uint256;

namespace interfaces {

/**
 * Interface giving clients (UI, wallet, etc) access to ReddID functionality.
 */
class ReddID
{
public:
    virtual ~ReddID() {}

    //! Return whether ReddID is initialized and active
    virtual bool isRunning() = 0;

    //! Start ReddID service
    virtual bool start() = 0;

    //! Stop ReddID service
    virtual bool stop() = 0;

    // Namespace operations
    //! Get all registered namespaces
    virtual std::vector<NamespaceInfo> getNamespaces() = 0;

    //! Get namespace information
    virtual bool getNamespaceInfo(const std::string& namespaceId, NamespaceInfo& result) = 0;

    //! Create namespace auction
    virtual bool createNamespaceAuction(const std::string& namespaceId, const CKeyID& creator,
                                       CAmount reservePrice, int durationDays, AuctionType type,
                                       uint256& auctionId) = 0;

    //! Bid on namespace auction
    virtual bool bidOnNamespaceAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) = 0;

    //! Finalize namespace auction
    virtual bool finalizeNamespaceAuction(const uint256& auctionId) = 0;

    //! Cancel namespace auction
    virtual bool cancelNamespaceAuction(const uint256& auctionId, const CKeyID& creator) = 0;

    //! Update namespace configuration
    virtual bool updateNamespaceConfig(const std::string& namespaceId, const NamespaceInfo& config, const CKeyID& owner) = 0;

    //! Renew namespace
    virtual bool renewNamespace(const std::string& namespaceId, const CKeyID& owner, int renewalDays) = 0;

    //! Transfer namespace ownership
    virtual bool transferNamespace(const std::string& namespaceId, const CKeyID& from, const CKeyID& to) = 0;

    //! Get all active namespace auctions
    virtual std::vector<AuctionInfo> getActiveNamespaceAuctions() = 0;

    // User ID operations
    //! Get user IDs in a namespace
    virtual std::vector<UserIDInfo> getUserIDs(const std::string& namespaceId) = 0;

    //! Get user ID information
    virtual bool getUserIDInfo(const std::string& name, const std::string& namespaceId, UserIDInfo& result) = 0;

    //! Create user ID auction
    virtual bool createUserIDAuction(const std::string& name, const std::string& namespaceId,
                                    const CKeyID& creator, CAmount reservePrice, int durationDays,
                                    AuctionType type, uint256& auctionId) = 0;

    //! Bid on user ID auction
    virtual bool bidOnUserIDAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) = 0;

    //! Finalize user ID auction
    virtual bool finalizeUserIDAuction(const uint256& auctionId) = 0;

    //! Cancel user ID auction
    virtual bool cancelUserIDAuction(const uint256& auctionId, const CKeyID& creator) = 0;

    //! Renew user ID
    virtual bool renewUserID(const std::string& name, const std::string& namespaceId, const CKeyID& owner) = 0;

    //! Transfer user ID
    virtual bool transferUserID(const std::string& name, const std::string& namespaceId,
                              const CKeyID& from, const CKeyID& to) = 0;

    //! Get active user ID auctions
    virtual std::vector<AuctionInfo> getActiveUserIDAuctions(const std::string& namespaceId = "") = 0;

    //! Get auction information
    virtual bool getAuctionInfo(const uint256& auctionId, AuctionInfo& result) = 0;

    //! Get auction bids
    virtual std::vector<BidInfo> getAuctionBids(const uint256& auctionId) = 0;

    // ReddID Profile operations
    //! Get profile information
    virtual bool getProfile(const std::string& reddId, ReddIDProfile& result) = 0;

    //! Update profile
    virtual bool updateProfile(const std::string& reddId, const ReddIDProfile& profile, const CKeyID& owner) = 0;

    //! Create social connection
    virtual bool createConnection(const std::string& fromReddId, const std::string& toReddId,
                                SocialConnectionType type, int visibility, const CKeyID& owner) = 0;

    //! Get social connections
    virtual std::vector<ReddIDConnection> getConnections(const std::string& reddId) = 0;

    //! Get reputation information
    virtual bool getReputation(const std::string& reddId, ReddIDReputation& result) = 0;

    //! Calculate reputation
    virtual bool calculateReputation(const std::string& reddId, ReddIDReputation& result) = 0;

    // Price calculation methods
    //! Calculate minimum price for namespace
    virtual CAmount calculateMinPriceForNamespace(const std::string& namespaceId) = 0;

    //! Calculate minimum price for user ID
    virtual CAmount calculateMinPriceForUserID(const std::string& name, const std::string& namespaceId) = 0;

    //! Calculate renewal fee for user ID
    virtual CAmount calculateRenewalFee(const std::string& name, const std::string& namespaceId) = 0;

    // Validation methods
    //! Validate ReddID format
    virtual bool validateReddID(const std::string& reddId) = 0;

    //! Validate namespace format
    virtual bool validateNamespace(const std::string& namespaceId) = 0;

    //! Validate user ID format
    virtual bool validateUserID(const std::string& name, const std::string& namespaceId) = 0;
};

std::unique_ptr<ReddID> MakeReddIDInterface(NodeContext& node);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_REDDID_H
