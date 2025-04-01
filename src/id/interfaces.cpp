// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/reddid.h>

#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <node/context.h>
#include <util/system.h>
#include <key.h>

#include <memory>
#include <string>
#include <vector>

namespace interfaces {

namespace {

class ReddIDImpl : public ReddID
{
public:
    explicit ReddIDImpl(NodeContext& node) : m_node(node) {}

    bool isRunning() override
    {
        return m_node.reddid && m_node.reddid->IsRunning();
    }

    bool start() override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->Start();
    }

    bool stop() override
    {
        if (!m_node.reddid) {
            return false;
        }
        m_node.reddid->Interrupt();
        return m_node.reddid->Stop();
    }

    // Namespace operations
    std::vector<NamespaceInfo> getNamespaces() override
    {
        if (!m_node.reddid || !m_node.reddid->GetNamespaceManager()) {
            return {};
        }
        return m_node.reddid->GetNamespaceManager()->GetNamespaces();
    }

    bool getNamespaceInfo(const std::string& namespaceId, NamespaceInfo& result) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->GetNamespaceInfo(namespaceId, result);
    }

    bool createNamespaceAuction(const std::string& namespaceId, const CKeyID& creator,
                               CAmount reservePrice, int durationDays, AuctionType type,
                               uint256& auctionId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->CreateNamespaceAuction(namespaceId, creator, reservePrice, 
                                                    durationDays, type, auctionId);
    }

    bool bidOnNamespaceAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->BidOnNamespaceAuction(auctionId, bidder, bidAmount);
    }

    bool finalizeNamespaceAuction(const uint256& auctionId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->FinalizeNamespaceAuction(auctionId);
    }

    bool cancelNamespaceAuction(const uint256& auctionId, const CKeyID& creator) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->CancelNamespaceAuction(auctionId, creator);
    }

    bool updateNamespaceConfig(const std::string& namespaceId, const NamespaceInfo& config, 
                              const CKeyID& owner) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->UpdateNamespaceConfig(namespaceId, config, owner);
    }

    bool renewNamespace(const std::string& namespaceId, const CKeyID& owner, int renewalDays) override
    {
        if (!m_node.reddid || !m_node.reddid->GetNamespaceManager()) {
            return false;
        }
        return m_node.reddid->GetNamespaceManager()->RenewNamespace(namespaceId, owner, renewalDays);
    }

    bool transferNamespace(const std::string& namespaceId, const CKeyID& from, const CKeyID& to) override
    {
        if (!m_node.reddid || !m_node.reddid->GetNamespaceManager()) {
            return false;
        }
        return m_node.reddid->GetNamespaceManager()->TransferNamespace(namespaceId, from, to);
    }

    std::vector<AuctionInfo> getActiveNamespaceAuctions() override
    {
        if (!m_node.reddid) {
            return {};
        }
        return m_node.reddid->GetActiveNamespaceAuctions();
    }

    // User ID operations
    std::vector<UserIDInfo> getUserIDs(const std::string& namespaceId) override
    {
        std::vector<UserIDInfo> result;
        if (!m_node.reddid || !m_node.reddid->GetReddIDDB()) {
            return result;
        }
        
        ReddIDDB* db = m_node.reddid->GetReddIDDB();
        std::vector<std::string> names;
        
        if (db->ListUserIDs(namespaceId, names)) {
            for (const auto& name : names) {
                UserIDInfo info;
                if (db->ReadUserID(name, namespaceId, info)) {
                    result.push_back(info);
                }
            }
        }
        
        return result;
    }

    bool getUserIDInfo(const std::string& name, const std::string& namespaceId, UserIDInfo& result) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->GetUserIDInfo(name, namespaceId, result);
    }

    bool createUserIDAuction(const std::string& name, const std::string& namespaceId,
                           const CKeyID& creator, CAmount reservePrice, int durationDays,
                           AuctionType type, uint256& auctionId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->CreateUserIDAuction(name, namespaceId, creator, 
                                                reservePrice, durationDays, type, auctionId);
    }

    bool bidOnUserIDAuction(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->BidOnUserIDAuction(auctionId, bidder, bidAmount);
    }

    bool finalizeUserIDAuction(const uint256& auctionId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->FinalizeUserIDAuction(auctionId);
    }

    bool cancelUserIDAuction(const uint256& auctionId, const CKeyID& creator) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->CancelUserIDAuction(auctionId, creator);
    }

    bool renewUserID(const std::string& name, const std::string& namespaceId, const CKeyID& owner) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->RenewUserID(name, namespaceId, owner);
    }

    bool transferUserID(const std::string& name, const std::string& namespaceId,
                       const CKeyID& from, const CKeyID& to) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->TransferUserID(name, namespaceId, from, to);
    }

    std::vector<AuctionInfo> getActiveUserIDAuctions(const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return {};
        }
        return m_node.reddid->GetActiveUserIDAuctions(namespaceId);
    }

    bool getAuctionInfo(const uint256& auctionId, AuctionInfo& result) override
    {
        if (!m_node.reddid || !m_node.reddid->GetAuctionManager()) {
            return false;
        }
        return m_node.reddid->GetAuctionManager()->GetAuctionInfo(auctionId, result);
    }

    std::vector<BidInfo> getAuctionBids(const uint256& auctionId) override
    {
        if (!m_node.reddid || !m_node.reddid->GetAuctionManager()) {
            return {};
        }
        return m_node.reddid->GetAuctionManager()->GetAuctionBids(auctionId);
    }

    // ReddID Profile operations
    bool getProfile(const std::string& reddId, ReddIDProfile& result) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->GetProfile(reddId, result);
    }

    bool updateProfile(const std::string& reddId, const ReddIDProfile& profile, const CKeyID& owner) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->UpdateProfile(reddId, profile, owner);
    }

    bool createConnection(const std::string& fromReddId, const std::string& toReddId,
                         SocialConnectionType type, int visibility, const CKeyID& owner) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->CreateConnection(fromReddId, toReddId, type, visibility, owner);
    }

    std::vector<ReddIDConnection> getConnections(const std::string& reddId) override
    {
        if (!m_node.reddid || !m_node.reddid->GetProfileManager()) {
            return {};
        }
        return m_node.reddid->GetProfileManager()->GetConnections(reddId);
    }

    bool getReputation(const std::string& reddId, ReddIDReputation& result) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->GetReputation(reddId, result);
    }

    bool calculateReputation(const std::string& reddId, ReddIDReputation& result) override
    {
        if (!m_node.reddid || !m_node.reddid->GetProfileManager()) {
            return false;
        }
        return m_node.reddid->GetProfileManager()->CalculateReputation(reddId, result);
    }

    // Price calculation methods
    CAmount calculateMinPriceForNamespace(const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return 0;
        }
        return m_node.reddid->CalculateMinPriceForNamespace(namespaceId);
    }

    CAmount calculateMinPriceForUserID(const std::string& name, const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return 0;
        }
        return m_node.reddid->CalculateMinPriceForUserID(name, namespaceId);
    }

    CAmount calculateRenewalFee(const std::string& name, const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return 0;
        }
        return m_node.reddid->CalculateRenewalFee(name, namespaceId);
    }

    // Validation methods
    bool validateReddID(const std::string& reddId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->ValidateReddID(reddId);
    }

    bool validateNamespace(const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->ValidateNamespace(namespaceId);
    }

    bool validateUserID(const std::string& name, const std::string& namespaceId) override
    {
        if (!m_node.reddid) {
            return false;
        }
        return m_node.reddid->ValidateUserID(name, namespaceId);
    }

private:
    NodeContext& m_node;
};

} // namespace

std::unique_ptr<ReddID> MakeReddIDInterface(NodeContext& node)
{
    return std::make_unique<ReddIDImpl>(node);
}

} // namespace interfaces
