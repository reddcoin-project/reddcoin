// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key_io.h>
#include <net.h>
#include <node/context.h>
#include <protocol.h>
#include <rpc/blockchain.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <sync.h>
#include <univalue.h>
#include <util/string.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <stdint.h>
#include <string>
#include <vector>

// The ReddID commands that need no wallet. The ones that do are in
// wallet/rpcreddid.cpp, which is only built with a wallet.
//
// The managers change their state from the chain tip, from the periodic task
// and from peer messages, always with cs_main held. Every command here takes
// cs_main before it reads or changes that state.

static RPCHelpMan getreddidnetworkinfo()
{
    return RPCHelpMan{"getreddidnetworkinfo",
                "\nReturns information about ReddID network status.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "enabled", "Whether this node has ReddID enabled"},
                        {RPCResult::Type::NUM, "connections", "Number of ReddID-capable peers"},
                        {RPCResult::Type::ARR, "peers", "List of ReddID peer IDs",
                            {
                                {RPCResult::Type::NUM, "", "peer node ID"}
                            }
                        },
                    }
                },
                RPCExamples{
                    HelpExampleCli("getreddidnetworkinfo", "")
                    + HelpExampleRpc("getreddidnetworkinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    UniValue obj(UniValue::VOBJ);

    // Check if ReddID is enabled
    bool reddidEnabled = (node.connman->GetLocalServices() & NODE_REDDID) != 0;
    obj.pushKV("enabled", reddidEnabled);

    // Get ReddID-capable peers
    UniValue peers(UniValue::VARR);
    int reddidPeerCount = 0;

    node.connman->ForEachNode([&](CNode* pnode) {
        if (pnode->nServices.load() & NODE_REDDID) {
            peers.push_back(pnode->GetId());
            reddidPeerCount++;
        }
    });

    obj.pushKV("connections", reddidPeerCount);
    obj.pushKV("peers", peers);

    return obj;
},
    };
}

static RPCHelpMan getnamespacelist()
{
    return RPCHelpMan{"getnamespacelist",
                "\nList all registered namespaces in the system.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "List of namespaces",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR, "id", "Namespace identifier"},
                            {RPCResult::Type::STR, "owner", "Owner's address"},
                            {RPCResult::Type::BOOL, "allow_numbers", "Whether numbers are allowed in user IDs"},
                            {RPCResult::Type::BOOL, "allow_hyphens", "Whether hyphens are allowed in user IDs"},
                            {RPCResult::Type::BOOL, "allow_underscores", "Whether underscores are allowed in user IDs"},
                            {RPCResult::Type::NUM, "min_length", "Minimum user ID length"},
                            {RPCResult::Type::NUM, "max_length", "Maximum user ID length"},
                            {RPCResult::Type::NUM, "renewal_period", "Days until renewal required"},
                            {RPCResult::Type::NUM, "grace_period", "Days in grace period"},
                            {RPCResult::Type::NUM, "expiration", "Timestamp when namespace expires"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespacelist", "")
                    + HelpExampleRpc("getnamespacelist", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    std::vector<NamespaceInfo> namespaces = namespaceManager->GetNamespaces();

    UniValue result(UniValue::VARR);

    for (const auto& ns : namespaces) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("id", ns.id);
        obj.pushKV("owner", EncodeDestination(PKHash(ns.owner)));
        obj.pushKV("allow_numbers", ns.allowNumbers);
        obj.pushKV("allow_hyphens", ns.allowHyphens);
        obj.pushKV("allow_underscores", ns.allowUnderscores);
        obj.pushKV("min_length", ns.minLength);
        obj.pushKV("max_length", ns.maxLength);
        obj.pushKV("renewal_period", ns.renewalPeriod);
        obj.pushKV("grace_period", ns.gracePeriod);
        obj.pushKV("namespace_revenue_pct", ns.namespaceRevenuePct);
        obj.pushKV("burn_pct", ns.burnPct);
        obj.pushKV("node_pct", ns.nodePct);
        obj.pushKV("dev_pct", ns.devPct);
        obj.pushKV("min_auction_duration", ns.minAuctionDuration);
        obj.pushKV("max_auction_duration", ns.maxAuctionDuration);
        obj.pushKV("min_bid_increment", ns.minBidIncrement);
        obj.pushKV("last_updated", (int64_t)ns.lastUpdated);
        obj.pushKV("expiration", (int64_t)ns.expiration);

        result.push_back(obj);
    }

    return result;
},
    };
}

static RPCHelpMan getnamespaceinfo()
{
    return RPCHelpMan{"getnamespaceinfo",
                "\nGet detailed information about a specific namespace.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "id", "Namespace identifier"},
                        {RPCResult::Type::STR, "owner", "Owner's address"},
                        {RPCResult::Type::BOOL, "allow_numbers", "Whether numbers are allowed in user IDs"},
                        {RPCResult::Type::BOOL, "allow_hyphens", "Whether hyphens are allowed in user IDs"},
                        {RPCResult::Type::BOOL, "allow_underscores", "Whether underscores are allowed in user IDs"},
                        {RPCResult::Type::NUM, "min_length", "Minimum user ID length"},
                        {RPCResult::Type::NUM, "max_length", "Maximum user ID length"},
                        {RPCResult::Type::NUM, "renewal_period", "Days until renewal required"},
                        {RPCResult::Type::NUM, "grace_period", "Days in grace period"},
                        {RPCResult::Type::NUM, "namespace_revenue_pct", "% of auctions going to namespace owner"},
                        {RPCResult::Type::NUM, "burn_pct", "% of auctions being burned"},
                        {RPCResult::Type::NUM, "node_pct", "% of auctions going to node operators"},
                        {RPCResult::Type::NUM, "dev_pct", "% of auctions going to development fund"},
                        {RPCResult::Type::NUM, "min_auction_duration", "Minimum auction duration in days"},
                        {RPCResult::Type::NUM, "max_auction_duration", "Maximum auction duration in days"},
                        {RPCResult::Type::NUM, "min_bid_increment", "Minimum bid increment percentage"},
                        {RPCResult::Type::NUM, "last_updated", "When configuration was last updated"},
                        {RPCResult::Type::NUM, "expiration", "When namespace ownership expires"},
                        {RPCResult::Type::ARR, "pricing_tiers", "Array of pricing tiers",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::NUM, "min_length", "Minimum length for this tier"},
                                {RPCResult::Type::NUM, "min_price", "Minimum price for names in this tier"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespaceinfo", "\"redd\"")
                    + HelpExampleRpc("getnamespaceinfo", "\"redd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string namespaceId = request.params[0].get_str();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    NamespaceInfo ns;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, ns)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("id", ns.id);
    result.pushKV("owner", EncodeDestination(PKHash(ns.owner)));
    result.pushKV("allow_numbers", ns.allowNumbers);
    result.pushKV("allow_hyphens", ns.allowHyphens);
    result.pushKV("allow_underscores", ns.allowUnderscores);
    result.pushKV("min_length", ns.minLength);
    result.pushKV("max_length", ns.maxLength);
    result.pushKV("renewal_period", ns.renewalPeriod);
    result.pushKV("grace_period", ns.gracePeriod);
    result.pushKV("namespace_revenue_pct", ns.namespaceRevenuePct);
    result.pushKV("burn_pct", ns.burnPct);
    result.pushKV("node_pct", ns.nodePct);
    result.pushKV("dev_pct", ns.devPct);
    result.pushKV("min_auction_duration", ns.minAuctionDuration);
    result.pushKV("max_auction_duration", ns.maxAuctionDuration);
    result.pushKV("min_bid_increment", ns.minBidIncrement);
    result.pushKV("last_updated", (int64_t)ns.lastUpdated);
    result.pushKV("expiration", (int64_t)ns.expiration);

    // Add pricing tiers
    std::vector<PricingTier> tiers = namespaceManager->GetPricingTiers(namespaceId);
    UniValue tiersArray(UniValue::VARR);

    for (const auto& tier : tiers) {
        UniValue tierObj(UniValue::VOBJ);
        tierObj.pushKV("min_length", tier.minLength);
        tierObj.pushKV("min_price", ValueFromAmount(tier.minPrice));
        tiersArray.push_back(tierObj);
    }

    result.pushKV("pricing_tiers", tiersArray);

    return result;
},
    };
}

static RPCHelpMan finalizenamespacesauction()
{
    return RPCHelpMan{"finalizenamespacesauction",
                "\nFinalize a namespace auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "winner", "The winner's address"},
                        {RPCResult::Type::STR_AMOUNT, "final_price", "The final price in RDD"},
                        {RPCResult::Type::NUM, "state", "The auction state (3=finalized)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("finalizenamespacesauction", "\"abcd1234\"")
                    + HelpExampleRpc("finalizenamespacesauction", "\"abcd1234\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Check if auction is active or ended
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active or ended");
    }

    // Check if auction has ended
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                          strprintf("Auction has not ended yet, %d seconds remaining",
                                   auction.endTime - currentTime));
    }

    // Finalize auction
    if (!namespaceManager->FinalizeNamespaceAuction(auctionId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to finalize auction");
    }

    // Get updated auction info
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("winner", EncodeDestination(PKHash(auction.currentBidder)));
    result.pushKV("final_price", ValueFromAmount(auction.currentBid));
    result.pushKV("state", (int)auction.state);

    return result;
},
    };
}

static RPCHelpMan getnamespaceauctionlist()
{
    return RPCHelpMan{"getnamespaceauctionlist",
                "\nList all namespace auctions in the system.\n",
                {
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, only show active auctions. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "List of namespace auctions",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                            {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                            {RPCResult::Type::STR, "creator", "The creator's address"},
                            {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                            {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                            {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                            {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                            {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                            {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium)"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespaceauctionlist", "")
                    + HelpExampleCli("getnamespaceauctionlist", "true")
                    + HelpExampleRpc("getnamespaceauctionlist", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool activeOnly = false;
    if (!request.params.empty()) {
        activeOnly = request.params[0].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    // Get auctions
    std::vector<AuctionInfo> auctions;
    if (activeOnly) {
        auctions = namespaceManager->GetActiveAuctions();
    } else {
        // We need to get all auctions - create a vector of auction IDs first
        ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();
        if (!reddidDB) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not initialized");
        }

        std::vector<uint256> auctionIds;
        if (!reddidDB->ListAuctions(auctionIds)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to list auctions");
        }

        // Filter to namespace auctions only by checking if the name field is empty
        for (const auto& auctionId : auctionIds) {
            AuctionInfo auction;
            if (namespaceManager->GetAuctionInfo(auctionId, auction)) {
                // Namespace auctions have empty name field
                if (auction.name.empty() && !auction.namespaceId.empty()) {
                    auctions.push_back(auction);
                }
            }
        }
    }

    UniValue result(UniValue::VARR);

    for (const auto& auction : auctions) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("auction_id", auction.auctionId.GetHex());
        obj.pushKV("namespace_id", auction.namespaceId);
        obj.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
        obj.pushKV("start_time", (int64_t)auction.startTime);
        obj.pushKV("end_time", (int64_t)auction.endTime);
        obj.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
        obj.pushKV("current_bid", ValueFromAmount(auction.currentBid));

        if (auction.currentBid > 0) {
            obj.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
        } else {
            obj.pushKV("current_bidder", "");
        }

        obj.pushKV("state", (int)auction.state);
        obj.pushKV("type", (int)auction.type);

        result.push_back(obj);
    }

    return result;
},
    };
}

static RPCHelpMan getnamespaceauctioninfo()
{
    return RPCHelpMan{"getnamespaceauctioninfo",
                "\nGet detailed information about a specific namespace auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, include bid history. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "creator", "The creator's address"},
                        {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                        {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                        {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                        {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                        {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                        {RPCResult::Type::STR_AMOUNT, "deposit_amount", "The deposit amount required for bidding in RDD"},
                        {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                        {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium)"},
                        {RPCResult::Type::NUM, "time_remaining", "Time remaining in seconds (if active)"},
                        {RPCResult::Type::ARR, "bids", "List of bids (if verbose=true)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                                {RPCResult::Type::STR, "bidder", "The bidder's address"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The bid amount in RDD"},
                                {RPCResult::Type::NUM, "time", "The bid timestamp"},
                                {RPCResult::Type::BOOL, "is_winner", "Whether this bid is the winner"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespaceauctioninfo", "\"abcd1234\"")
                    + HelpExampleCli("getnamespaceauctioninfo", "\"abcd1234\" true")
                    + HelpExampleRpc("getnamespaceauctioninfo", "\"abcd1234\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    bool verbose = false;
    if (request.params.size() > 1) {
        verbose = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Verify this is a namespace auction
    if (!auction.name.empty() || auction.namespaceId.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This is not a namespace auction");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auction.auctionId.GetHex());
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
    result.pushKV("start_time", (int64_t)auction.startTime);
    result.pushKV("end_time", (int64_t)auction.endTime);
    result.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
    result.pushKV("current_bid", ValueFromAmount(auction.currentBid));

    if (auction.currentBid > 0) {
        result.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
    } else {
        result.pushKV("current_bidder", "");
    }

    result.pushKV("deposit_amount", ValueFromAmount(auction.depositAmount));
    result.pushKV("state", (int)auction.state);
    result.pushKV("type", (int)auction.type);

    // Calculate time remaining if auction is active
    int64_t currentTime = GetTime();
    if (auction.state == AUCTION_ACTIVE && auction.endTime > currentTime) {
        result.pushKV("time_remaining", (int64_t)(auction.endTime - currentTime));
    } else {
        result.pushKV("time_remaining", 0);
    }

    // Add bid history if verbose
    if (verbose) {
        // Get the auction manager to access bids
        AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
        if (!auctionManager) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
        }

        std::vector<BidInfo> bids = auctionManager->GetAuctionBids(auctionId);

        // Sort bids by time (most recent first)
        std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
            return a.bidTime > b.bidTime;
        });

        UniValue bidsArray(UniValue::VARR);

        for (const auto& bid : bids) {
            UniValue bidObj(UniValue::VOBJ);
            bidObj.pushKV("bid_id", bid.bidId.GetHex());
            bidObj.pushKV("bidder", EncodeDestination(PKHash(bid.bidder)));
            bidObj.pushKV("amount", ValueFromAmount(bid.bidAmount));
            bidObj.pushKV("time", (int64_t)bid.bidTime);
            bidObj.pushKV("is_winner", bid.isWinner);

            bidsArray.push_back(bidObj);
        }

        result.pushKV("bids", bidsArray);
    }

    return result;
},
    };
}

static RPCHelpMan getnamespacesauctions()
{
    return RPCHelpMan{"getnamespacesauctions",
                "\nList all namespace auctions in the system.\n",
                {
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, only show active auctions. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "List of namespace auctions",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                            {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                            {RPCResult::Type::STR, "creator", "The creator's address"},
                            {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                            {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                            {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                            {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                            {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                            {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium)"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespacesauctions", "")
                    + HelpExampleCli("getnamespacesauctions", "true")
                    + HelpExampleRpc("getnamespacesauctions", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // This is a direct implementation of the function instead of calling getnamespaceauctionlist
    bool activeOnly = false;
    if (!request.params.empty()) {
        activeOnly = request.params[0].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    // Get auctions
    std::vector<AuctionInfo> auctions;
    if (activeOnly) {
        auctions = namespaceManager->GetActiveAuctions();
    } else {
        // We need to get all auctions - create a vector of auction IDs first
        ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();
        if (!reddidDB) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not initialized");
        }

        std::vector<uint256> auctionIds;
        if (!reddidDB->ListAuctions(auctionIds)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to list auctions");
        }

        // Filter to namespace auctions only by checking if the name field is empty
        for (const auto& auctionId : auctionIds) {
            AuctionInfo auction;
            if (namespaceManager->GetAuctionInfo(auctionId, auction)) {
                // Namespace auctions have empty name field
                if (auction.name.empty() && !auction.namespaceId.empty()) {
                    auctions.push_back(auction);
                }
            }
        }
    }

    UniValue result(UniValue::VARR);

    for (const auto& auction : auctions) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("auction_id", auction.auctionId.GetHex());
        obj.pushKV("namespace_id", auction.namespaceId);
        obj.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
        obj.pushKV("start_time", (int64_t)auction.startTime);
        obj.pushKV("end_time", (int64_t)auction.endTime);
        obj.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
        obj.pushKV("current_bid", ValueFromAmount(auction.currentBid));

        if (auction.currentBid > 0) {
            obj.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
        } else {
            obj.pushKV("current_bidder", "");
        }

        obj.pushKV("state", (int)auction.state);
        obj.pushKV("type", (int)auction.type);

        result.push_back(obj);
    }

    return result;
},
    };
}

static RPCHelpMan getnamespacesauctionbids()
{
    return RPCHelpMan{"getnamespacesauctionbids",
                "\nGet all bids for a specific namespace auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"sort_by", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Sort order: 'time' (default), 'amount'"},
                    {"sort_dir", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Sort direction: 'desc' (default), 'asc'"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::NUM, "total_bids", "The total number of bids"},
                        {RPCResult::Type::STR_AMOUNT, "highest_bid", "The highest bid amount in RDD"},
                        {RPCResult::Type::ARR, "bids", "List of bids",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                                {RPCResult::Type::STR, "bidder", "The bidder's address"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The bid amount in RDD"},
                                {RPCResult::Type::STR_AMOUNT, "deposit", "The deposit amount in RDD"},
                                {RPCResult::Type::NUM, "time", "The bid timestamp"},
                                {RPCResult::Type::BOOL, "is_winner", "Whether this bid is the winner"},
                                {RPCResult::Type::BOOL, "refunded", "Whether the deposit was refunded (for non-winners)"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getnamespacesauctionbids", "\"abcd1234\"")
                    + HelpExampleCli("getnamespacesauctionbids", "\"abcd1234\" \"amount\" \"desc\"")
                    + HelpExampleRpc("getnamespacesauctionbids", "\"abcd1234\", \"amount\", \"desc\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Optional sorting parameters
    std::string sortBy = "time";  // Default sort by time
    std::string sortDir = "desc"; // Default newest first

    if (request.params.size() > 1) {
        sortBy = request.params[1].get_str();
        if (sortBy != "time" && sortBy != "amount") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "sort_by must be 'time' or 'amount'");
        }
    }

    if (request.params.size() > 2) {
        sortDir = request.params[2].get_str();
        if (sortDir != "asc" && sortDir != "desc") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "sort_dir must be 'asc' or 'desc'");
        }
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!auctionManager || !namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Verify this is a namespace auction
    if (!auction.name.empty() || auction.namespaceId.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This is not a namespace auction");
    }

    // Get auction bids
    std::vector<BidInfo> bids = auctionManager->GetAuctionBids(auctionId);

    // Sort the bids
    if (sortBy == "time") {
        if (sortDir == "asc") {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidTime < b.bidTime;
            });
        } else {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidTime > b.bidTime;
            });
        }
    } else if (sortBy == "amount") {
        if (sortDir == "asc") {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidAmount < b.bidAmount;
            });
        } else {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidAmount > b.bidAmount;
            });
        }
    }

    // Find highest bid amount
    CAmount highestBid = 0;
    for (const auto& bid : bids) {
        if (bid.bidAmount > highestBid) {
            highestBid = bid.bidAmount;
        }
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("total_bids", (int)bids.size());
    result.pushKV("highest_bid", ValueFromAmount(highestBid));

    // Add bids array
    UniValue bidsArray(UniValue::VARR);
    for (const auto& bid : bids) {
        UniValue bidObj(UniValue::VOBJ);
        bidObj.pushKV("bid_id", bid.bidId.GetHex());
        bidObj.pushKV("bidder", EncodeDestination(PKHash(bid.bidder)));
        bidObj.pushKV("amount", ValueFromAmount(bid.bidAmount));
        bidObj.pushKV("deposit", ValueFromAmount(bid.depositAmount));
        bidObj.pushKV("time", (int64_t)bid.bidTime);
        bidObj.pushKV("is_winner", bid.isWinner);
        bidObj.pushKV("refunded", bid.refunded);

        bidsArray.push_back(bidObj);
    }

    result.pushKV("bids", bidsArray);

    return result;
},
    };
}

static RPCHelpMan getuseridlist()
{
    return RPCHelpMan{"getuseridlist",
                "\nList user IDs in a specific namespace.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "List of user IDs",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR, "name", "User ID name"},
                            {RPCResult::Type::STR, "namespace_id", "Namespace identifier"},
                            {RPCResult::Type::STR, "owner", "Owner's address"},
                            {RPCResult::Type::NUM, "registration_time", "Registration timestamp"},
                            {RPCResult::Type::NUM, "expiration_time", "Expiration timestamp"},
                            {RPCResult::Type::NUM, "transaction_count", "Number of transactions using this ID"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuseridlist", "\"redd\"")
                    + HelpExampleRpc("getuseridlist", "\"redd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string namespaceId = request.params[0].get_str();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!namespaceManager || !reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    // Get user IDs
    std::vector<std::string> names;
    if (!reddidDB->ListUserIDs(namespaceId, names)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to list user IDs");
    }

    UniValue result(UniValue::VARR);

    for (const auto& name : names) {
        UserIDInfo userID;
        if (reddidDB->ReadUserID(name, namespaceId, userID)) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("name", userID.name);
            obj.pushKV("namespace_id", userID.namespaceId);
            obj.pushKV("owner", EncodeDestination(PKHash(userID.owner)));
            obj.pushKV("registration_time", (int64_t)userID.registrationTime);
            obj.pushKV("expiration_time", (int64_t)userID.expirationTime);
            obj.pushKV("last_transaction", (int64_t)userID.lastTransaction);
            obj.pushKV("transaction_count", userID.transactionCount);

            result.push_back(obj);
        }
    }

    return result;
},
    };
}

static RPCHelpMan getuseridinfo()
{
    return RPCHelpMan{"getuseridinfo",
                "\nGet detailed information about a specific user ID.\n",
                {
                    {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The user ID name"},
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "User ID name"},
                        {RPCResult::Type::STR, "namespace_id", "Namespace identifier"},
                        {RPCResult::Type::STR, "owner", "Owner's address"},
                        {RPCResult::Type::NUM, "registration_time", "Registration timestamp"},
                        {RPCResult::Type::NUM, "expiration_time", "Expiration timestamp"},
                        {RPCResult::Type::NUM, "last_transaction", "Timestamp of last transaction using this ID"},
                        {RPCResult::Type::NUM, "transaction_count", "Number of transactions using this ID"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuseridinfo", "\"alice\" \"redd\"")
                    + HelpExampleRpc("getuseridinfo", "\"alice\", \"redd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string name = request.params[0].get_str();
    std::string namespaceId = request.params[1].get_str();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not initialized");
    }

    // Get user ID info
    UserIDInfo userID;
    if (!reddidDB->ReadUserID(name, namespaceId, userID)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "User ID not found");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("name", userID.name);
    result.pushKV("namespace_id", userID.namespaceId);
    result.pushKV("owner", EncodeDestination(PKHash(userID.owner)));
    result.pushKV("registration_time", (int64_t)userID.registrationTime);
    result.pushKV("expiration_time", (int64_t)userID.expirationTime);
    result.pushKV("last_transaction", (int64_t)userID.lastTransaction);
    result.pushKV("transaction_count", userID.transactionCount);

    return result;
},
    };
}

static RPCHelpMan getuserauctionlist()
{
    return RPCHelpMan{"getuserauctionlist",
                "\nList all user ID auctions in the system.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter by namespace ID"},
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, only show active auctions. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "List of user ID auctions",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                            {RPCResult::Type::STR, "name", "The user ID name"},
                            {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                            {RPCResult::Type::STR, "creator", "The creator's address"},
                            {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                            {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                            {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                            {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                            {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                            {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium, 2=verified, 3=renewal)"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuserauctionlist", "")
                    + HelpExampleCli("getuserauctionlist", "\"redd\"")
                    + HelpExampleCli("getuserauctionlist", "\"redd\" true")
                    + HelpExampleRpc("getuserauctionlist", "\"redd\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string namespaceId;
    bool activeOnly = false;

    if (request.params.size() > 0) {
        namespaceId = request.params[0].get_str();
    }

    if (request.params.size() > 1) {
        activeOnly = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();

    if (!auctionManager || !reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Get auctions
    std::vector<AuctionInfo> auctions;
    if (activeOnly) {
        // If namespace is specified, get active auctions for that namespace
        if (!namespaceId.empty()) {
            auctions = auctionManager->GetAuctionsByNamespace(namespaceId);
        } else {
            // Get all active auctions
            auctions = auctionManager->GetActiveAuctions();
        }
    } else {
        // Get all auctions and filter
        std::vector<uint256> auctionIds;
        if (!reddidDB->ListAuctions(auctionIds)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to list auctions");
        }

        // Filter to user ID auctions only
        for (const auto& auctionId : auctionIds) {
            AuctionInfo auction;
            if (auctionManager->GetAuctionInfo(auctionId, auction)) {
                // User ID auctions have non-empty name field
                if (!auction.name.empty() && !auction.namespaceId.empty()) {
                    // If namespace filter is specified, apply it
                    if (namespaceId.empty() || auction.namespaceId == namespaceId) {
                        auctions.push_back(auction);
                    }
                }
            }
        }
    }

    UniValue result(UniValue::VARR);

    for (const auto& auction : auctions) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("auction_id", auction.auctionId.GetHex());
        obj.pushKV("name", auction.name);
        obj.pushKV("namespace_id", auction.namespaceId);
        obj.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
        obj.pushKV("start_time", (int64_t)auction.startTime);
        obj.pushKV("end_time", (int64_t)auction.endTime);
        obj.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
        obj.pushKV("current_bid", ValueFromAmount(auction.currentBid));

        if (auction.currentBid > 0) {
            obj.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
        } else {
            obj.pushKV("current_bidder", "");
        }

        obj.pushKV("state", (int)auction.state);
        obj.pushKV("type", (int)auction.type);

        // Calculate time remaining if auction is active
        int64_t currentTime = GetTime();
        if (auction.state == AUCTION_ACTIVE && auction.endTime > currentTime) {
            obj.pushKV("time_remaining", (int64_t)(auction.endTime - currentTime));
        } else {
            obj.pushKV("time_remaining", 0);
        }

        result.push_back(obj);
    }

    return result;
},
    };
}

static RPCHelpMan getuserauctioninfo()
{
    return RPCHelpMan{"getuserauctioninfo",
                "\nGet detailed information about a specific user ID auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, include bid history. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "creator", "The creator's address"},
                        {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                        {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                        {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                        {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                        {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                        {RPCResult::Type::STR_AMOUNT, "deposit_amount", "The deposit amount required for bidding in RDD"},
                        {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                        {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium, 2=verified, 3=renewal)"},
                        {RPCResult::Type::NUM, "time_remaining", "Time remaining in seconds (if active)"},
                        {RPCResult::Type::OBJ, "namespace_info", "Information about the parent namespace",
                        {
                            {RPCResult::Type::STR, "id", "Namespace identifier"},
                            {RPCResult::Type::STR, "owner", "Owner's address"},
                            {RPCResult::Type::NUM, "min_length", "Minimum user ID length"},
                            {RPCResult::Type::NUM, "max_length", "Maximum user ID length"},
                            {RPCResult::Type::NUM, "renewal_period", "Days until renewal required"},
                            {RPCResult::Type::NUM, "grace_period", "Days in grace period"}
                        }},
                        {RPCResult::Type::ARR, "bids", "List of bids (if verbose=true)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                                {RPCResult::Type::STR, "bidder", "The bidder's address"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The bid amount in RDD"},
                                {RPCResult::Type::NUM, "time", "The bid timestamp"},
                                {RPCResult::Type::BOOL, "is_winner", "Whether this bid is the winner"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuserauctioninfo", "\"abcd1234\"")
                    + HelpExampleCli("getuserauctioninfo", "\"abcd1234\" true")
                    + HelpExampleRpc("getuserauctioninfo", "\"abcd1234\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    bool verbose = false;
    if (request.params.size() > 1) {
        verbose = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!auctionManager || !namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Get auction info
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Verify this is a user ID auction
    if (auction.name.empty() || auction.namespaceId.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This is not a user ID auction");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auction.auctionId.GetHex());
    result.pushKV("name", auction.name);
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
    result.pushKV("start_time", (int64_t)auction.startTime);
    result.pushKV("end_time", (int64_t)auction.endTime);
    result.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
    result.pushKV("current_bid", ValueFromAmount(auction.currentBid));

    if (auction.currentBid > 0) {
        result.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
    } else {
        result.pushKV("current_bidder", "");
    }

    result.pushKV("deposit_amount", ValueFromAmount(auction.depositAmount));
    result.pushKV("state", (int)auction.state);
    result.pushKV("type", (int)auction.type);

    // Calculate time remaining if auction is active
    int64_t currentTime = GetTime();
    if (auction.state == AUCTION_ACTIVE && auction.endTime > currentTime) {
        result.pushKV("time_remaining", (int64_t)(auction.endTime - currentTime));
    } else {
        result.pushKV("time_remaining", 0);
    }

    // Add namespace info
    NamespaceInfo namespaceInfo;
    if (namespaceManager->GetNamespaceInfo(auction.namespaceId, namespaceInfo)) {
        UniValue nsInfo(UniValue::VOBJ);
        nsInfo.pushKV("id", namespaceInfo.id);
        nsInfo.pushKV("owner", EncodeDestination(PKHash(namespaceInfo.owner)));
        nsInfo.pushKV("allow_numbers", namespaceInfo.allowNumbers);
        nsInfo.pushKV("allow_hyphens", namespaceInfo.allowHyphens);
        nsInfo.pushKV("allow_underscores", namespaceInfo.allowUnderscores);
        nsInfo.pushKV("min_length", namespaceInfo.minLength);
        nsInfo.pushKV("max_length", namespaceInfo.maxLength);
        nsInfo.pushKV("renewal_period", namespaceInfo.renewalPeriod);
        nsInfo.pushKV("grace_period", namespaceInfo.gracePeriod);

        result.pushKV("namespace_info", nsInfo);
    }

    // Add bid history if verbose
    if (verbose) {
        std::vector<BidInfo> bids = auctionManager->GetAuctionBids(auctionId);

        // Sort bids by time (most recent first)
        std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
            return a.bidTime > b.bidTime;
        });

        UniValue bidsArray(UniValue::VARR);

        for (const auto& bid : bids) {
            UniValue bidObj(UniValue::VOBJ);
            bidObj.pushKV("bid_id", bid.bidId.GetHex());
            bidObj.pushKV("bidder", EncodeDestination(PKHash(bid.bidder)));
            bidObj.pushKV("amount", ValueFromAmount(bid.bidAmount));
            bidObj.pushKV("time", (int64_t)bid.bidTime);
            bidObj.pushKV("is_winner", bid.isWinner);

            bidsArray.push_back(bidObj);
        }

        result.pushKV("bids", bidsArray);
    }

    return result;
},
    };
}

static RPCHelpMan finalizeuseridauction()
{
    return RPCHelpMan{"finalizeuseridauction",
                "\nFinalize a user ID auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "winner", "The winner's address"},
                        {RPCResult::Type::STR_AMOUNT, "final_price", "The final price in RDD"},
                        {RPCResult::Type::NUM, "state", "The auction state (3=finalized)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("finalizeuseridauction", "\"abcd1234\"")
                    + HelpExampleRpc("finalizeuseridauction", "\"abcd1234\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();

    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
    }

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Get auction info
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Check if auction can be finalized
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active or ended");
    }

    // Check if auction has ended
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                          strprintf("Auction has not ended yet, %d seconds remaining",
                                   auction.endTime - currentTime));
    }

    // Finalize auction
    if (!reddIDManager->FinalizeUserIDAuction(auctionId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to finalize auction");
    }

    // Get updated auction info
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("name", auction.name);
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("winner", EncodeDestination(PKHash(auction.currentBidder)));
    result.pushKV("final_price", ValueFromAmount(auction.currentBid));
    result.pushKV("state", (int)auction.state);

    return result;
},
    };
}

static RPCHelpMan getuseridauctions()
{
    return RPCHelpMan{"getuseridauctions",
                "\nList all user ID auctions in the system.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter by namespace ID"},
                    {"active_only", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, only show active auctions. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "List of user ID auctions",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                            {RPCResult::Type::STR, "name", "The user ID name"},
                            {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                            {RPCResult::Type::STR, "creator", "The creator's address"},
                            {RPCResult::Type::NUM, "start_time", "Start time as Unix timestamp"},
                            {RPCResult::Type::NUM, "end_time", "End time as Unix timestamp"},
                            {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                            {RPCResult::Type::STR_AMOUNT, "current_bid", "The current highest bid in RDD"},
                            {RPCResult::Type::STR, "current_bidder", "The current highest bidder's address (if there is a bid)"},
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=settlement, 5=completed, 6=defaulted, 7=re-auction, 8=canceled)"},
                            {RPCResult::Type::NUM, "type", "The auction type (0=standard, 1=premium, 2=verified, 3=renewal)"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuseridauctions", "")
                    + HelpExampleCli("getuseridauctions", "\"redd\"")
                    + HelpExampleCli("getuseridauctions", "\"redd\" true")
                    + HelpExampleRpc("getuseridauctions", "\"redd\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // This is a direct implementation of the function instead of calling getuserauctionlist
    std::string namespaceId;
    bool activeOnly = false;

    if (request.params.size() > 0) {
        namespaceId = request.params[0].get_str();
    }

    if (request.params.size() > 1) {
        activeOnly = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();

    if (!auctionManager || !reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Get auctions
    std::vector<AuctionInfo> auctions;
    if (activeOnly) {
        // If namespace is specified, get active auctions for that namespace
        if (!namespaceId.empty()) {
            auctions = auctionManager->GetAuctionsByNamespace(namespaceId);
        } else {
            // Get all active auctions
            auctions = auctionManager->GetActiveAuctions();
        }
    } else {
        // Get all auctions and filter
        std::vector<uint256> auctionIds;
        if (!reddidDB->ListAuctions(auctionIds)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to list auctions");
        }

        // Filter to user ID auctions only
        for (const auto& auctionId : auctionIds) {
            AuctionInfo auction;
            if (auctionManager->GetAuctionInfo(auctionId, auction)) {
                // User ID auctions have non-empty name field
                if (!auction.name.empty() && !auction.namespaceId.empty()) {
                    // If namespace filter is specified, apply it
                    if (namespaceId.empty() || auction.namespaceId == namespaceId) {
                        auctions.push_back(auction);
                    }
                }
            }
        }
    }

    UniValue result(UniValue::VARR);

    for (const auto& auction : auctions) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("auction_id", auction.auctionId.GetHex());
        obj.pushKV("name", auction.name);
        obj.pushKV("namespace_id", auction.namespaceId);
        obj.pushKV("creator", EncodeDestination(PKHash(auction.creator)));
        obj.pushKV("start_time", (int64_t)auction.startTime);
        obj.pushKV("end_time", (int64_t)auction.endTime);
        obj.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
        obj.pushKV("current_bid", ValueFromAmount(auction.currentBid));

        if (auction.currentBid > 0) {
            obj.pushKV("current_bidder", EncodeDestination(PKHash(auction.currentBidder)));
        } else {
            obj.pushKV("current_bidder", "");
        }

        obj.pushKV("state", (int)auction.state);
        obj.pushKV("type", (int)auction.type);

        // Calculate time remaining if auction is active
        int64_t currentTime = GetTime();
        if (auction.state == AUCTION_ACTIVE && auction.endTime > currentTime) {
            obj.pushKV("time_remaining", (int64_t)(auction.endTime - currentTime));
        } else {
            obj.pushKV("time_remaining", 0);
        }

        result.push_back(obj);
    }

    return result;
},
    };
}

static RPCHelpMan getuseridauctionbids()
{
    return RPCHelpMan{"getuseridauctionbids",
                "\nGet all bids for a specific user ID auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"sort_by", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Sort order: 'time' (default), 'amount'"},
                    {"sort_dir", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Sort direction: 'desc' (default), 'asc'"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::NUM, "total_bids", "The total number of bids"},
                        {RPCResult::Type::STR_AMOUNT, "highest_bid", "The highest bid amount in RDD"},
                        {RPCResult::Type::ARR, "bids", "List of bids",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                                {RPCResult::Type::STR, "bidder", "The bidder's address"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The bid amount in RDD"},
                                {RPCResult::Type::STR_AMOUNT, "deposit", "The deposit amount in RDD"},
                                {RPCResult::Type::NUM, "time", "The bid timestamp"},
                                {RPCResult::Type::BOOL, "is_winner", "Whether this bid is the winner"},
                                {RPCResult::Type::BOOL, "refunded", "Whether the deposit was refunded (for non-winners)"},
                                {RPCResult::Type::STR, "bidder_reddid", "The bidder's ReddID (if they have one)"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuseridauctionbids", "\"abcd1234\"")
                    + HelpExampleCli("getuseridauctionbids", "\"abcd1234\" \"amount\" \"desc\"")
                    + HelpExampleRpc("getuseridauctionbids", "\"abcd1234\", \"amount\", \"desc\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Optional sorting parameters
    std::string sortBy = "time";  // Default sort by time
    std::string sortDir = "desc"; // Default newest first

    if (request.params.size() > 1) {
        sortBy = request.params[1].get_str();
        if (sortBy != "time" && sortBy != "amount") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "sort_by must be 'time' or 'amount'");
        }
    }

    if (request.params.size() > 2) {
        sortDir = request.params[2].get_str();
        if (sortDir != "asc" && sortDir != "desc") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "sort_dir must be 'asc' or 'desc'");
        }
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Get auction info
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Verify this is a user ID auction
    if (auction.name.empty() || auction.namespaceId.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This is not a user ID auction");
    }

    // Get auction bids
    std::vector<BidInfo> bids = auctionManager->GetAuctionBids(auctionId);

    // Sort the bids
    if (sortBy == "time") {
        if (sortDir == "asc") {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidTime < b.bidTime;
            });
        } else {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidTime > b.bidTime;
            });
        }
    } else if (sortBy == "amount") {
        if (sortDir == "asc") {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidAmount < b.bidAmount;
            });
        } else {
            std::sort(bids.begin(), bids.end(), [](const BidInfo& a, const BidInfo& b) {
                return a.bidAmount > b.bidAmount;
            });
        }
    }

    // Find highest bid amount
    CAmount highestBid = 0;
    for (const auto& bid : bids) {
        if (bid.bidAmount > highestBid) {
            highestBid = bid.bidAmount;
        }
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("name", auction.name);
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("total_bids", (int)bids.size());
    result.pushKV("highest_bid", ValueFromAmount(highestBid));

    // Add bids array
    UniValue bidsArray(UniValue::VARR);
    for (const auto& bid : bids) {
        UniValue bidObj(UniValue::VOBJ);
        bidObj.pushKV("bid_id", bid.bidId.GetHex());
        bidObj.pushKV("bidder", EncodeDestination(PKHash(bid.bidder)));
        bidObj.pushKV("amount", ValueFromAmount(bid.bidAmount));
        bidObj.pushKV("deposit", ValueFromAmount(bid.depositAmount));
        bidObj.pushKV("time", (int64_t)bid.bidTime);
        bidObj.pushKV("is_winner", bid.isWinner);
        bidObj.pushKV("refunded", bid.refunded);

        // Add bidder's ReddID if any
        if (!bid.bidderReddId.empty()) {
            bidObj.pushKV("bidder_reddid", bid.bidderReddId);
        } else if (profileManager) {
            // Try to look up the bidder's ReddID if not already provided
            std::vector<std::string> reddIds;
            if (profileManager->GetProfilesByOwner(bid.bidder, reddIds) && !reddIds.empty()) {
                bidObj.pushKV("bidder_reddid", reddIds[0]); // Use the first ReddID found
            }
        }

        bidsArray.push_back(bidObj);
    }

    result.pushKV("bids", bidsArray);

    return result;
},
    };
}

static RPCHelpMan getuserconnections()
{
    return RPCHelpMan{"getuserconnections",
                "\nGet all social connections for a specific ReddID.\n",
                {
                    {"reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The ReddID to get connections for"},
                    {"connection_type", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
                     "Filter by connection type (0=follow, 1=friend, 2=endorse, 3=block). Default: all types"},
                    {"direction", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "Connection direction to show: 'outgoing' (default), 'incoming', or 'both'"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "reddid", "The ReddID"},
                        {RPCResult::Type::ARR, "outgoing_connections", "List of outgoing connections",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "to_reddid", "The target ReddID"},
                                {RPCResult::Type::NUM, "connection_type",
                                 "Connection type (0=follow, 1=friend, 2=endorse, 3=block)"},
                                {RPCResult::Type::STR, "type_name", "Text representation of connection type"},
                                {RPCResult::Type::NUM, "creation_time", "When the connection was created"},
                                {RPCResult::Type::NUM, "last_interaction", "Timestamp of last interaction with this connection"},
                                {RPCResult::Type::NUM, "visibility", "Privacy setting (0=public, 1=friends, 2=private)"},
                                {RPCResult::Type::STR, "metadata", "Additional connection metadata"}
                            }}
                        }},
                        {RPCResult::Type::ARR, "incoming_connections", "List of incoming connections (if requested)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "from_reddid", "The source ReddID"},
                                {RPCResult::Type::NUM, "connection_type",
                                 "Connection type (0=follow, 1=friend, 2=endorse, 3=block)"},
                                {RPCResult::Type::STR, "type_name", "Text representation of connection type"},
                                {RPCResult::Type::NUM, "creation_time", "When the connection was created"},
                                {RPCResult::Type::NUM, "last_interaction", "Timestamp of last interaction with this connection"},
                                {RPCResult::Type::NUM, "visibility", "Privacy setting (0=public, 1=friends, 2=private)"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuserconnections", "\"alice\"")
                    + HelpExampleCli("getuserconnections", "\"alice\" 1")
                    + HelpExampleCli("getuserconnections", "\"alice\" null \"both\"")
                    + HelpExampleRpc("getuserconnections", "\"alice\", 1, \"both\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string reddId = request.params[0].get_str();

    // Optional connection type filter
    int connectionTypeFilter = -1; // -1 means no filter
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        connectionTypeFilter = request.params[1].get_int();
        if (connectionTypeFilter < CONNECTION_FOLLOW || connectionTypeFilter > CONNECTION_BLOCK) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                              "Invalid connection type, must be between 0 and 3");
        }
    }

    // Optional direction
    std::string direction = "outgoing"; // default: show outgoing connections only
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        direction = request.params[2].get_str();
        if (direction != "outgoing" && direction != "incoming" && direction != "both") {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                              "direction must be 'outgoing', 'incoming', or 'both'");
        }
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!profileManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Profile manager not initialized");
    }

    // Check if ReddID exists
    ReddIDProfile profile;
    if (!profileManager->GetProfile(reddId, profile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "ReddID not found");
    }

    // Get outgoing connections
    std::vector<ReddIDConnection> outgoingConnections;
    if (direction == "outgoing" || direction == "both") {
        outgoingConnections = profileManager->GetConnections(reddId);
    }

    // For incoming connections, we need to search for connections where toReddId matches
    std::vector<ReddIDConnection> incomingConnections;
    if (direction == "incoming" || direction == "both") {
        // This would require a specialized function in ProfileManager to get incoming connections
        // For now, we would need to load all connections and filter
        std::vector<std::string> allReddIds;
        if (profileManager->GetDB()->ListProfiles(allReddIds)) {
            for (const auto& fromId : allReddIds) {
                if (fromId == reddId) continue; // Skip self

                std::vector<ReddIDConnection> connections = profileManager->GetConnections(fromId);
                for (const auto& conn : connections) {
                    if (conn.toReddId == reddId) {
                        incomingConnections.push_back(conn);
                    }
                }
            }
        }
    }

    // Apply connection type filter if specified
    if (connectionTypeFilter >= 0) {
        auto filterFunc = [connectionTypeFilter](const ReddIDConnection& conn) {
            return conn.connectionType != static_cast<SocialConnectionType>(connectionTypeFilter);
        };

        outgoingConnections.erase(
            std::remove_if(outgoingConnections.begin(), outgoingConnections.end(), filterFunc),
            outgoingConnections.end()
        );

        incomingConnections.erase(
            std::remove_if(incomingConnections.begin(), incomingConnections.end(), filterFunc),
            incomingConnections.end()
        );
    }

    // Helper function to convert connection type to string
    auto getConnectionTypeName = [](SocialConnectionType type) -> std::string {
        switch (type) {
            case CONNECTION_FOLLOW: return "follow";
            case CONNECTION_FRIEND: return "friend";
            case CONNECTION_ENDORSE: return "endorse";
            case CONNECTION_BLOCK: return "block";
            default: return "unknown";
        }
    };

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reddId);

    if (direction == "outgoing" || direction == "both") {
        UniValue outArray(UniValue::VARR);
        for (const auto& conn : outgoingConnections) {
            UniValue connObj(UniValue::VOBJ);
            connObj.pushKV("to_reddid", conn.toReddId);
            connObj.pushKV("connection_type", static_cast<int>(conn.connectionType));
            connObj.pushKV("type_name", getConnectionTypeName(conn.connectionType));
            connObj.pushKV("creation_time", (int64_t)conn.creationTime);
            connObj.pushKV("last_interaction", (int64_t)conn.lastInteraction);
            connObj.pushKV("visibility", conn.visibility);
            connObj.pushKV("metadata", conn.metadata);

            outArray.push_back(connObj);
        }
        result.pushKV("outgoing_connections", outArray);
    }

    if (direction == "incoming" || direction == "both") {
        UniValue inArray(UniValue::VARR);
        for (const auto& conn : incomingConnections) {
            UniValue connObj(UniValue::VOBJ);
            connObj.pushKV("from_reddid", conn.fromReddId);
            connObj.pushKV("connection_type", static_cast<int>(conn.connectionType));
            connObj.pushKV("type_name", getConnectionTypeName(conn.connectionType));
            connObj.pushKV("creation_time", (int64_t)conn.creationTime);
            connObj.pushKV("last_interaction", (int64_t)conn.lastInteraction);
            connObj.pushKV("visibility", conn.visibility);

            inArray.push_back(connObj);
        }
        result.pushKV("incoming_connections", inArray);
    }

    return result;
},
    };
}

static RPCHelpMan getuserreputation()
{
    return RPCHelpMan{"getuserreputation",
                "\nGet the reputation information for a specific ReddID user.\n",
                {
                    {"reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The ReddID to get reputation for"},
                    {"detailed", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "If true, include detailed breakdown of reputation components. Default: false"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "reddid", "The ReddID"},
                        {RPCResult::Type::NUM, "overall_score", "Overall reputation score (0-100)"},
                        {RPCResult::Type::NUM, "last_calculated", "Timestamp when reputation was last calculated"},
                        {RPCResult::Type::NUM, "longevity_score", "Score component for account age (if detailed=true)"},
                        {RPCResult::Type::NUM, "reliability_score", "Score component for deposit reliability (if detailed=true)"},
                        {RPCResult::Type::NUM, "engagement_score", "Score component for community engagement (if detailed=true)"},
                        {RPCResult::Type::NUM, "verification_score", "Score component for verification depth (if detailed=true)"},
                        {RPCResult::Type::NUM, "auction_score", "Score component for auction behavior (if detailed=true)"},
                        {RPCResult::Type::STR, "owner", "The ReddID owner's address"},
                        {RPCResult::Type::OBJ, "profile_summary", "Basic profile information",
                        {
                            {RPCResult::Type::STR, "display_name", "User-selected display name"},
                            {RPCResult::Type::NUM, "verification_status", "Verification level (0-3)"},
                            {RPCResult::Type::NUM, "creation_time", "When the ReddID was created"},
                            {RPCResult::Type::BOOL, "active", "Whether the ReddID is currently active"}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuserreputation", "\"alice\"")
                    + HelpExampleCli("getuserreputation", "\"alice\" true")
                    + HelpExampleRpc("getuserreputation", "\"alice\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string reddId = request.params[0].get_str();
    bool detailed = false;

    if (request.params.size() > 1) {
        detailed = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!profileManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Profile manager not initialized");
    }

    // Check if ReddID exists
    ReddIDProfile profile;
    if (!profileManager->GetProfile(reddId, profile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "ReddID not found");
    }

    // Get reputation data
    ReddIDReputation reputation;
    if (!profileManager->GetReputation(reddId, reputation)) {
        // If no reputation exists yet, create a default one
        reputation.reddId = reddId;
        reputation.overallScore = 50.0; // Default starting score
        reputation.longevityScore = 0.0;
        reputation.reliabilityScore = 0.0;
        reputation.engagementScore = 0.0;
        reputation.verificationScore = 0.0;
        reputation.auctionScore = 50.0;
        reputation.lastCalculated = GetTime();
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reputation.reddId);
    result.pushKV("overall_score", reputation.overallScore);
    result.pushKV("last_calculated", (int64_t)reputation.lastCalculated);

    // If detailed, include score components
    if (detailed) {
        result.pushKV("longevity_score", reputation.longevityScore);
        result.pushKV("reliability_score", reputation.reliabilityScore);
        result.pushKV("engagement_score", reputation.engagementScore);
        result.pushKV("verification_score", reputation.verificationScore);
        result.pushKV("auction_score", reputation.auctionScore);
    }

    // Add owner and basic profile info
    result.pushKV("owner", EncodeDestination(PKHash(profile.owner)));

    UniValue profileSummary(UniValue::VOBJ);
    profileSummary.pushKV("display_name", profile.displayName);
    profileSummary.pushKV("verification_status", (int)profile.verificationStatus);
    profileSummary.pushKV("creation_time", (int64_t)profile.creationTime);
    profileSummary.pushKV("active", profile.active);

    result.pushKV("profile_summary", profileSummary);

    return result;
},
    };
}

static RPCHelpMan getuserreputationhistory()
{
    return RPCHelpMan{"getuserreputationhistory",
                "\nGet reputation history for a specific ReddID.\n",
                {
                    {"reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The ReddID to get reputation history for"},
                    {"count", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Number of historical entries to retrieve (default: 10)"},
                    {"start_time", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Start time as Unix timestamp (default: current time)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "reddid", "The ReddID"},
                        {RPCResult::Type::NUM, "current_score", "Current overall reputation score (0-100)"},
                        {RPCResult::Type::ARR, "history", "Historical reputation data",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::NUM, "timestamp", "Timestamp when the reputation was calculated"},
                                {RPCResult::Type::NUM, "overall_score", "Overall reputation score (0-100)"},
                                {RPCResult::Type::NUM, "longevity_score", "Score component for account age"},
                                {RPCResult::Type::NUM, "reliability_score", "Score component for deposit reliability"},
                                {RPCResult::Type::NUM, "engagement_score", "Score component for community engagement"},
                                {RPCResult::Type::NUM, "verification_score", "Score component for verification depth"},
                                {RPCResult::Type::NUM, "auction_score", "Score component for auction behavior"},
                                {RPCResult::Type::STR, "calculator_signatures", "Number of validators that confirmed the score"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getuserreputationhistory", "\"alice\"")
                    + HelpExampleCli("getuserreputationhistory", "\"alice\" 20")
                    + HelpExampleCli("getuserreputationhistory", "\"alice\" 20 1703721600")
                    + HelpExampleRpc("getuserreputationhistory", "\"alice\", 20, 1703721600")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string reddId = request.params[0].get_str();

    // Optional parameters
    int count = 10; // Default: retrieve last 10 entries
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        count = request.params[1].get_int();
        if (count <= 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Count must be positive");
        }
    }

    int64_t startTime = GetTime(); // Default: current time
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        startTime = request.params[2].get_int64();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!profileManager || !reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    // Check if ReddID exists
    ReddIDProfile profile;
    if (!profileManager->GetProfile(reddId, profile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "ReddID not found");
    }

    // Get current reputation
    ReddIDReputation currentRep;
    if (!profileManager->GetReputation(reddId, currentRep)) {
        // If no reputation exists yet, we'll create a default one
        currentRep.reddId = reddId;
        currentRep.overallScore = 50.0;
        currentRep.lastCalculated = GetTime();
    }

    // Get reputation history from database
    std::vector<ReddIDReputation> reputationHistory;
    if (!reddidDB->GetReputationHistory(reddId, reputationHistory, startTime, count)) {
        // No error if history doesn't exist, just return empty array
        reputationHistory.clear();
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reddId);
    result.pushKV("current_score", currentRep.overallScore);

    UniValue historyArray(UniValue::VARR);
    for (const auto& rep : reputationHistory) {
        UniValue historyEntry(UniValue::VOBJ);
        historyEntry.pushKV("timestamp", (int64_t)rep.lastCalculated);
        historyEntry.pushKV("overall_score", rep.overallScore);
        historyEntry.pushKV("longevity_score", rep.longevityScore);
        historyEntry.pushKV("reliability_score", rep.reliabilityScore);
        historyEntry.pushKV("engagement_score", rep.engagementScore);
        historyEntry.pushKV("verification_score", rep.verificationScore);
        historyEntry.pushKV("auction_score", rep.auctionScore);

        // Count the number of calculator signatures
        int signatureCount = 0;
        if (!rep.calculatorSignatures.empty()) {
            try {
                // The signatures are stored as a JSON array, so we try to parse it
                UniValue signaturesArray;
                signaturesArray.read(rep.calculatorSignatures);
                signatureCount = signaturesArray.size();
            } catch (const std::exception&) {
                // If parsing fails, just return 0
                signatureCount = 0;
            }
        }
        historyEntry.pushKV("calculator_signatures", ToString(signatureCount));

        historyArray.push_back(historyEntry);
    }

    result.pushKV("history", historyArray);

    return result;
},
    };
}

static RPCHelpMan reddidgetdepositlock()
{
    return RPCHelpMan{"reddidgetdepositlock",
                "\nQuery a deposit lock by UTXO.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The UTXO transaction id"},
                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The UTXO output index"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The UTXO transaction id"},
                        {RPCResult::Type::NUM, "vout", "The UTXO output index"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                        {RPCResult::Type::STR, "bidder", "The bidder's address"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "The deposit amount"},
                        {RPCResult::Type::NUM, "lock_height", "Block height when locked"},
                        {RPCResult::Type::NUM, "release_height", "Block height when released (0 if locked)"},
                        {RPCResult::Type::STR, "state", "The deposit state"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidgetdepositlock", "\"abcd1234...\" 1")
                    + HelpExampleRpc("reddidgetdepositlock", "\"abcd1234...\", 1")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    uint256 txid = ParseHashV(request.params[0], "txid");
    int vout = request.params[1].get_int();

    COutPoint outpoint(txid, vout);
    DepositLock lock;
    if (!auctionManager->GetDepositLock(outpoint, lock))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Deposit lock not found for specified UTXO");

    // Map state enum to string
    std::string stateStr;
    switch (lock.state) {
        case DEPOSIT_LOCKED_ACTIVE:  stateStr = "active";    break;
        case DEPOSIT_RELEASED_LOST:  stateStr = "released";  break;
        case DEPOSIT_CONSUMED_WON:   stateStr = "consumed";  break;
        case DEPOSIT_FORFEITED:      stateStr = "forfeited"; break;
        default:                     stateStr = "unknown";   break;
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", lock.utxo.hash.GetHex());
    result.pushKV("vout", (int)lock.utxo.n);
    result.pushKV("auction_id", lock.auctionId.GetHex());
    result.pushKV("bid_id", lock.bidId.GetHex());
    result.pushKV("bidder", EncodeDestination(PKHash(lock.bidder)));
    result.pushKV("amount", ValueFromAmount(lock.amount));
    result.pushKV("lock_height", lock.lockHeight);
    result.pushKV("release_height", lock.releaseHeight);
    result.pushKV("state", stateStr);
    return result;
},
    };
}

static RPCHelpMan reddidgetauctionconfig()
{
    return RPCHelpMan{"reddidgetauctionconfig",
                "\nQuery auction configuration parameters.\n",
                {
                    {"config_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Config ID: \"namespace\", \"userid\", or \"reddid\" (default: \"userid\")"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "config_id", "The configuration identifier"},
                        {RPCResult::Type::NUM, "deposit_percentage", "Deposit percentage of bid amount"},
                        {RPCResult::Type::NUM, "settlement_window_blocks", "Blocks allowed for settlement"},
                        {RPCResult::Type::NUM, "anti_snipe_threshold_seconds", "Anti-snipe threshold in seconds"},
                        {RPCResult::Type::NUM, "anti_snipe_extension_seconds", "Anti-snipe extension in seconds"},
                        {RPCResult::Type::NUM, "min_duration_days", "Minimum auction duration in days"},
                        {RPCResult::Type::NUM, "max_duration_days", "Maximum auction duration in days"},
                        {RPCResult::Type::NUM, "min_bid_increment_pct", "Minimum bid increment percentage"},
                        {RPCResult::Type::STR, "community_fund_address", "Address for forfeited deposits"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidgetauctionconfig", "")
                    + HelpExampleCli("reddidgetauctionconfig", "\"namespace\"")
                    + HelpExampleRpc("reddidgetauctionconfig", "\"userid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    std::string configId = "userid";
    if (!request.params[0].isNull())
        configId = request.params[0].get_str();

    AuctionConfig config;
    if (!auctionManager->GetAuctionConfig(configId, config)) {
        // Fall back to defaults
        if (configId == "namespace")
            config = AuctionConfig::NamespaceDefaults();
        else if (configId == "reddid")
            config = AuctionConfig::ReddIDDefaults();
        else
            config = AuctionConfig::UserIDDefaults();
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("config_id", config.configId);
    result.pushKV("deposit_percentage", config.depositPercentage);
    result.pushKV("settlement_window_blocks", config.settlementWindowBlocks);
    result.pushKV("anti_snipe_threshold_seconds", config.antiSnipeThresholdSeconds);
    result.pushKV("anti_snipe_extension_seconds", config.antiSnipeExtensionSeconds);
    result.pushKV("min_duration_days", config.minDurationDays);
    result.pushKV("max_duration_days", config.maxDurationDays);
    result.pushKV("min_bid_increment_pct", config.minBidIncrementPct);
    result.pushKV("community_fund_address", config.communityFundAddress);
    return result;
},
    };
}

static RPCHelpMan reddidsetauctionconfig()
{
    return RPCHelpMan{"reddidsetauctionconfig",
                "\nUpdate a single auction configuration parameter.\n",
                {
                    {"config_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Config ID: \"namespace\", \"userid\", or \"reddid\""},
                    {"param", RPCArg::Type::STR, RPCArg::Optional::NO,
                     "Parameter name: depositPercentage, settlementWindowBlocks, antiSnipeThresholdSeconds, "
                     "antiSnipeExtensionSeconds, minDurationDays, maxDurationDays, minBidIncrementPct"},
                    {"value", RPCArg::Type::NUM, RPCArg::Optional::NO, "The new value"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "success", "Whether the update succeeded"},
                        {RPCResult::Type::STR, "config_id", "The configuration identifier"},
                        {RPCResult::Type::STR, "param", "The parameter that was updated"},
                        {RPCResult::Type::STR, "value", "The new value"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidsetauctionconfig", "\"userid\" \"depositPercentage\" 20")
                    + HelpExampleRpc("reddidsetauctionconfig", "\"userid\", \"depositPercentage\", 20")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    LOCK(cs_main);

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    std::string configId = request.params[0].get_str();
    std::string param = request.params[1].get_str();

    // Get existing config or create default
    AuctionConfig config;
    if (!auctionManager->GetAuctionConfig(configId, config)) {
        if (configId == "namespace")
            config = AuctionConfig::NamespaceDefaults();
        else if (configId == "reddid")
            config = AuctionConfig::ReddIDDefaults();
        else
            config = AuctionConfig::UserIDDefaults();
        config.configId = configId;
    }

    std::string valueStr;
    if (param == "depositPercentage") {
        int val = request.params[2].get_int();
        if (val < 0 || val > 100)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "depositPercentage must be between 0 and 100");
        config.depositPercentage = val;
        valueStr = ToString(val);
    } else if (param == "settlementWindowBlocks") {
        config.settlementWindowBlocks = request.params[2].get_int();
        valueStr = ToString(config.settlementWindowBlocks);
    } else if (param == "antiSnipeThresholdSeconds") {
        config.antiSnipeThresholdSeconds = request.params[2].get_int();
        valueStr = ToString(config.antiSnipeThresholdSeconds);
    } else if (param == "antiSnipeExtensionSeconds") {
        config.antiSnipeExtensionSeconds = request.params[2].get_int();
        valueStr = ToString(config.antiSnipeExtensionSeconds);
    } else if (param == "minDurationDays") {
        config.minDurationDays = request.params[2].get_int();
        valueStr = ToString(config.minDurationDays);
    } else if (param == "maxDurationDays") {
        config.maxDurationDays = request.params[2].get_int();
        valueStr = ToString(config.maxDurationDays);
    } else if (param == "minBidIncrementPct") {
        config.minBidIncrementPct = request.params[2].get_int();
        valueStr = ToString(config.minBidIncrementPct);
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown config parameter: %s", param));
    }

    config.updatedAt = GetTime();
    if (!auctionManager->SetAuctionConfig(config))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to save auction config");

    UniValue result(UniValue::VOBJ);
    result.pushKV("success", true);
    result.pushKV("config_id", configId);
    result.pushKV("param", param);
    result.pushKV("value", valueStr);
    return result;
},
    };
}

// Register the ReddID RPC commands that need no wallet
void RegisterReddIDRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] = {
    // Reddid Network
    { "reddid", &getreddidnetworkinfo,             },

    // Namespace commands
    { "reddid", &getnamespacelist,                 },
    { "reddid", &getnamespaceinfo,                 },
    { "reddid", &getnamespaceauctionlist,          },
    { "reddid", &getnamespaceauctioninfo,          },
    { "reddid", &finalizenamespacesauction,        },
    { "reddid", &getnamespacesauctions,            },
    { "reddid", &getnamespacesauctionbids,         },

    // User ID commands
    { "reddid", &getuseridlist,                    },
    { "reddid", &getuseridinfo,                    },
    { "reddid", &getuserauctionlist,               },
    { "reddid", &getuserauctioninfo,               },
    { "reddid", &finalizeuseridauction,            },
    { "reddid", &getuseridauctions,                },
    { "reddid", &getuseridauctionbids,             },

    // Auction commands
    { "reddid", &getuserconnections,               },
    { "reddid", &getuserreputation,                },
    { "reddid", &getuserreputationhistory,         },

    // Deposit and settlement commands
    { "reddid", &reddidgetdepositlock,             },
    { "reddid", &reddidgetauctionconfig,           },
    { "reddid", &reddidsetauctionconfig,           },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
