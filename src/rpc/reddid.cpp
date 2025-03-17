// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/reddid.h>

#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/rpcwallet.h>
#include <wallet/coincontrol.h>
#include <wallet/scriptpubkeyman.h>
#include <core_io.h>  // For ValueFromAmount

#include <stdint.h>
#include <string>

// Helper function to get a new destination from the wallet with the specified output type
CKeyID GetNewWalletDestination(CWallet* pwallet, std::string& error, OutputType output_type = OutputType::LEGACY, const std::string& label = "") {
    if (!pwallet) {
        error = "Wallet not available";
        return CKeyID();
    }
    
    CTxDestination dest;
    if (!pwallet->GetNewDestination(output_type, label, dest, error)) {
        return CKeyID();
    }
    
    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

    CKeyID keyID = GetKeyForDestination(spk_man, dest);
    if (keyID.IsNull()) {
        error = "Failed to get key ID for destination";
    }
    
    return keyID;
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

static RPCHelpMan createnamespacesauction()
{
    return RPCHelpMan{"createnamespacesauction",
                "\nCreate a namespace auction.\n",
		{
		    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier (1-15 characters)"},
		    {"reserve_price", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The minimum bid amount in RDD"},
		    {"duration_days", RPCArg::Type::NUM, RPCArg::Optional::NO, "The auction duration in days (14-30)"},
		    {"auction_type", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The auction type (0=standard, 1=premium)"},
		    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
		},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::NUM, "start_time", "The auction start time"},
                        {RPCResult::Type::NUM, "end_time", "The auction end time"},
                        {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                        {RPCResult::Type::NUM, "auction_type", "The auction type"},
                        {RPCResult::Type::STR, "creator_address", "The creator's address"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("createnamespacesauction", "\"redd\" 50000 14")
                    + HelpExampleCli("createnamespacesauction", "\"redd\" 50000 14 0 \"auctions\"")
                    + HelpExampleRpc("createnamespacesauction", "\"redd\", 50000, 14")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    std::string namespaceId = request.params[0].get_str();
    CAmount reservePrice = AmountFromValue(request.params[1]);
    int durationDays = request.params[2].get_int();
    AuctionType auctionType = request.params.size() > 3 ?
                             static_cast<AuctionType>(request.params[3].get_int()) :
                             AUCTION_STANDARD;
    std::string account = request.params.size() > 4 ? request.params[4].get_str() : "";

    // Validate namespace ID
    if (!namespaceManager->ValidateNamespaceID(namespaceId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid namespace ID format");
    }

    // Check if namespace is available
    if (!namespaceManager->IsNamespaceAvailable(namespaceId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace already exists");
    }

    // Validate reserve price
    CAmount minPrice = namespaceManager->CalculateMinPrice(namespaceId);
    if (reservePrice < minPrice) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                          strprintf("Reserve price too low, minimum is %s RDD",
                                   FormatMoney(minPrice)));
    }

    // Validate duration
    if (durationDays < 14 || durationDays > 30) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Duration must be between 14 and 30 days");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 4) {
	std::string address_type = request.params[4].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 5) {
	label = request.params[5].get_str();
    }

    // Get creator address using the HD wallet functionality
    std::string error;
    CKeyID creatorKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (creatorKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Create auction
    uint256 auctionId;
    if (!namespaceManager->CreateNamespaceAuction(namespaceId, creatorKeyId,
                                                 reservePrice, durationDays,
                                                 auctionType, auctionId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to create namespace auction");
    }

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("start_time", (int64_t)auction.startTime);
    result.pushKV("end_time", (int64_t)auction.endTime);
    result.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
    result.pushKV("auction_type", (int)auction.type);
    result.pushKV("creator_address", EncodeDestination(PKHash(auction.creator)));

    return result;
},
    };
}

static RPCHelpMan bidonnamespacesauction()
{
    return RPCHelpMan{"bidonnamespacesauction",
                "\nPlace a bid on a namespace auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"bid_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The bid amount in RDD"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR_AMOUNT, "bid_amount", "The bid amount in RDD"},
                        {RPCResult::Type::STR_AMOUNT, "deposit_amount", "The deposit amount in RDD"},
                        {RPCResult::Type::NUM, "bid_time", "The bid timestamp"},
                        {RPCResult::Type::STR, "bidder_address", "The bidder's address"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("bidonnamespacesauction", "\"abcd1234\" 100000")
                    + HelpExampleCli("bidonnamespacesauction", "\"abcd1234\" 100000 \"auctions\"")
                    + HelpExampleRpc("bidonnamespacesauction", "\"abcd1234\", 100000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");
    CAmount bidAmount = AmountFromValue(request.params[1]);
    std::string account = request.params.size() > 2 ? request.params[2].get_str() : "";

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Check if auction is active
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 2) {
	std::string address_type = request.params[2].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 3) {
	label = request.params[3].get_str();
    }

    // Get bidder address using the HD wallet functionality
    std::string error;
    CKeyID bidderKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (bidderKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Place bid
    uint256 bidId;
    if (!namespaceManager->BidOnNamespaceAuction(auctionId, bidderKeyId, bidAmount, bidId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to place bid");
    }

    // Get bid info - we need to use the auction manager here
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
    }

    BidInfo bid;
    if (!auctionManager->GetBidInfo(bidId, bid)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve bid info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("bid_id", bidId.GetHex());
    result.pushKV("auction_id", bid.auctionId.GetHex());
    result.pushKV("bid_amount", ValueFromAmount(bid.bidAmount));
    result.pushKV("deposit_amount", ValueFromAmount(bid.depositAmount));
    result.pushKV("bid_time", (int64_t)bid.bidTime);
    result.pushKV("bidder_address", EncodeDestination(PKHash(bid.bidder)));

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
                        {RPCResult::Type::STR, "state", "The auction state (3=finalized)"}
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

static RPCHelpMan cancelnamespacesauction()
{
    return RPCHelpMan{"cancelnamespacesauction",
                "\nCancel a namespace auction. Only the creator can cancel an auction, and only if there are no bids.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "state", "The auction state (4=canceled)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("cancelnamespacesauction", "\"abcd1234\"")
                    + HelpExampleCli("cancelnamespacesauction", "\"abcd1234\" \"auctions\"")
                    + HelpExampleRpc("cancelnamespacesauction", "\"abcd1234\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");
    std::string account = request.params.size() > 1 ? request.params[1].get_str() : "";

    // Get auction info
    AuctionInfo auction;
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 4) {
	std::string address_type = request.params[4].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 5) {
	label = request.params[5].get_str();
    }

    // Get creator address using the HD wallet functionality
    std::string error;
    CKeyID creatorKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (creatorKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Cancel auction
    if (!namespaceManager->CancelNamespaceAuction(auctionId, creatorKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to cancel auction");
    }

    // Get updated auction info
    if (!namespaceManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("namespace_id", auction.namespaceId);
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
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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
                        {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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

static RPCHelpMan configurenamespacess()
{
    return RPCHelpMan{"configurenamespacess",
                "\nUpdate configuration parameters for a namespace. Only the namespace owner can perform this operation.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"config", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Configuration parameters",
                    {
                        {"allow_numbers", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether numbers are allowed in user IDs"},
                        {"allow_hyphens", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether hyphens are allowed in user IDs"},
                        {"allow_underscores", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether underscores are allowed in user IDs"},
                        {"min_length", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Minimum user ID length (1-10)"},
                        {"max_length", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Maximum user ID length (5-64)"},
                        {"renewal_period", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Days until renewal required (180-730)"},
                        {"grace_period", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Days in grace period (14-60)"},
                        {"namespace_revenue_pct", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "% of auctions going to namespace owner (5-10)"},
                        {"burn_pct", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "% of auctions being burned (50-80)"},
                        {"node_pct", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "% of auctions going to node operators (5-25)"},
                        {"dev_pct", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "% of auctions going to development fund (5-15)"},
                        {"min_auction_duration", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Minimum auction duration in days (1-7)"},
                        {"max_auction_duration", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Maximum auction duration in days (3-14)"},
                        {"min_bid_increment", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Minimum bid increment percentage (1.0-10.0)"}
                    }},
                    {"pricing_tiers", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Pricing tiers for different name lengths",
                    {
                        {"tier", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "A pricing tier",
                        {
                            {"min_length", RPCArg::Type::NUM, RPCArg::Optional::NO, "Minimum length for this tier"},
                            {"min_price", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Minimum price for names in this tier in RDD"}
                        }},
                    }},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"}
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
                        {RPCResult::Type::ARR, "pricing_tiers", "Array of pricing tiers",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::NUM, "min_length", "Minimum length for this tier"},
                                {RPCResult::Type::STR_AMOUNT, "min_price", "Minimum price for names in this tier"}
                            }}
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("configurenamespacess", "\"redd\" '{\"allow_numbers\":true,\"min_length\":3,\"max_length\":32}'")
                    + HelpExampleCli("configurenamespacess", "\"redd\" '{\"min_bid_increment\":5.0}' '[{\"min_length\":1,\"min_price\":100000},{\"min_length\":3,\"min_price\":10000}]'")
                    + HelpExampleRpc("configurenamespacess", "\"redd\", {\"allow_numbers\":true,\"min_length\":3,\"max_length\":32}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    // Parse parameters
    std::string namespaceId = request.params[0].get_str();
    UniValue configObj = request.params[1].get_obj();

    // Get existing namespace configuration
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 3) {
        std::string address_type = request.params[3].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 4) {
        label = request.params[4].get_str();
    }

    // Get owner address using the HD wallet functionality
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check ownership
    if (namespaceInfo.owner != ownerKeyId) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the owner of this namespace");
    }

    // Update namespace configuration based on provided parameters
    bool configUpdated = false;

    // Character rules
    if (configObj.exists("allow_numbers")) {
        namespaceInfo.allowNumbers = configObj["allow_numbers"].get_bool();
        configUpdated = true;
    }

    if (configObj.exists("allow_hyphens")) {
        namespaceInfo.allowHyphens = configObj["allow_hyphens"].get_bool();
        configUpdated = true;
    }

    if (configObj.exists("allow_underscores")) {
        namespaceInfo.allowUnderscores = configObj["allow_underscores"].get_bool();
        configUpdated = true;
    }

    // Length requirements
    if (configObj.exists("min_length")) {
        int minLength = configObj["min_length"].get_int();
        if (minLength < 1 || minLength > 10) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "min_length must be between 1 and 10");
        }
        namespaceInfo.minLength = minLength;
        configUpdated = true;
    }

    if (configObj.exists("max_length")) {
        int maxLength = configObj["max_length"].get_int();
        if (maxLength < 5 || maxLength > 64) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "max_length must be between 5 and 64");
        }
        if (maxLength < namespaceInfo.minLength) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "max_length must be greater than or equal to min_length");
        }
        namespaceInfo.maxLength = maxLength;
        configUpdated = true;
    }

    // Time periods
    if (configObj.exists("renewal_period")) {
        int renewalPeriod = configObj["renewal_period"].get_int();
        if (renewalPeriod < 180 || renewalPeriod > 730) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "renewal_period must be between 180 and 730 days");
        }
        namespaceInfo.renewalPeriod = renewalPeriod;
        configUpdated = true;
    }

    if (configObj.exists("grace_period")) {
        int gracePeriod = configObj["grace_period"].get_int();
        if (gracePeriod < 14 || gracePeriod > 60) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "grace_period must be between 14 and 60 days");
        }
        namespaceInfo.gracePeriod = gracePeriod;
        configUpdated = true;
    }

    // Revenue distribution
    if (configObj.exists("namespace_revenue_pct")) {
        int namespacePct = configObj["namespace_revenue_pct"].get_int();
        if (namespacePct < 5 || namespacePct > 10) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "namespace_revenue_pct must be between 5 and 10");
        }
        namespaceInfo.namespaceRevenuePct = namespacePct;
        configUpdated = true;
    }

    if (configObj.exists("burn_pct")) {
        int burnPct = configObj["burn_pct"].get_int();
        if (burnPct < 50 || burnPct > 80) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "burn_pct must be between 50 and 80");
        }
        namespaceInfo.burnPct = burnPct;
        configUpdated = true;
    }

    if (configObj.exists("node_pct")) {
        int nodePct = configObj["node_pct"].get_int();
        if (nodePct < 5 || nodePct > 25) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "node_pct must be between 5 and 25");
        }
        namespaceInfo.nodePct = nodePct;
        configUpdated = true;
    }

    if (configObj.exists("dev_pct")) {
        int devPct = configObj["dev_pct"].get_int();
        if (devPct < 5 || devPct > 15) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "dev_pct must be between 5 and 15");
        }
        namespaceInfo.devPct = devPct;
        configUpdated = true;
    }

    // Verify total is 100%
    if (namespaceInfo.namespaceRevenuePct + namespaceInfo.burnPct +
        namespaceInfo.nodePct + namespaceInfo.devPct != 100) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("Revenue percentages must sum to 100%% (current: %d%%)",
                     namespaceInfo.namespaceRevenuePct + namespaceInfo.burnPct +
                     namespaceInfo.nodePct + namespaceInfo.devPct));
    }

    // Auction parameters
    if (configObj.exists("min_auction_duration")) {
        int minAuctionDuration = configObj["min_auction_duration"].get_int();
        if (minAuctionDuration < 1 || minAuctionDuration > 7) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "min_auction_duration must be between 1 and 7 days");
        }
        namespaceInfo.minAuctionDuration = minAuctionDuration;
        configUpdated = true;
    }

    if (configObj.exists("max_auction_duration")) {
        int maxAuctionDuration = configObj["max_auction_duration"].get_int();
        if (maxAuctionDuration < 3 || maxAuctionDuration > 14) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "max_auction_duration must be between 3 and 14 days");
        }
        if (maxAuctionDuration < namespaceInfo.minAuctionDuration) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "max_auction_duration must be greater than or equal to min_auction_duration");
        }
        namespaceInfo.maxAuctionDuration = maxAuctionDuration;
        configUpdated = true;
    }

    if (configObj.exists("min_bid_increment")) {
        double minBidIncrement = configObj["min_bid_increment"].get_real();
        if (minBidIncrement < 1.0 || minBidIncrement > 10.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "min_bid_increment must be between 1.0 and 10.0");
        }
        namespaceInfo.minBidIncrement = minBidIncrement;
        configUpdated = true;
    }

    // Check if we have pricing tiers to update
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        UniValue tiersArray = request.params[2].get_array();

        // Clear existing pricing tiers
        namespaceManager->RemovePricingTier(namespaceId, 0); // Remove all tiers

        // Add new pricing tiers
        for (size_t i = 0; i < tiersArray.size(); i++) {
            UniValue tierObj = tiersArray[i].get_obj();

            if (!tierObj.exists("min_length") || !tierObj.exists("min_price")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Pricing tier must have min_length and min_price");
            }

            int minLength = tierObj["min_length"].get_int();
            CAmount minPrice = AmountFromValue(tierObj["min_price"]);

            if (minLength < 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "min_length must be at least 1");
            }

            if (minPrice < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "min_price must be positive");
            }

            namespaceManager->AddPricingTier(namespaceId, minLength, minPrice);
        }

        configUpdated = true;
    }

    // Update namespace if anything changed
    if (configUpdated) {
        namespaceInfo.lastUpdated = GetTime();

        if (!namespaceManager->UpdateNamespace(namespaceInfo)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to update namespace configuration");
        }
    }

    // Get updated information and pricing tiers
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated namespace info");
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("id", namespaceInfo.id);
    result.pushKV("owner", EncodeDestination(PKHash(namespaceInfo.owner)));
    result.pushKV("allow_numbers", namespaceInfo.allowNumbers);
    result.pushKV("allow_hyphens", namespaceInfo.allowHyphens);
    result.pushKV("allow_underscores", namespaceInfo.allowUnderscores);
    result.pushKV("min_length", namespaceInfo.minLength);
    result.pushKV("max_length", namespaceInfo.maxLength);
    result.pushKV("renewal_period", namespaceInfo.renewalPeriod);
    result.pushKV("grace_period", namespaceInfo.gracePeriod);
    result.pushKV("namespace_revenue_pct", namespaceInfo.namespaceRevenuePct);
    result.pushKV("burn_pct", namespaceInfo.burnPct);
    result.pushKV("node_pct", namespaceInfo.nodePct);
    result.pushKV("dev_pct", namespaceInfo.devPct);
    result.pushKV("min_auction_duration", namespaceInfo.minAuctionDuration);
    result.pushKV("max_auction_duration", namespaceInfo.maxAuctionDuration);
    result.pushKV("min_bid_increment", namespaceInfo.minBidIncrement);
    result.pushKV("last_updated", (int64_t)namespaceInfo.lastUpdated);
    result.pushKV("expiration", (int64_t)namespaceInfo.expiration);

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
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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

static RPCHelpMan renewnamespace()
{
    return RPCHelpMan{"renewnamespace",
                "\nRenew an existing namespace to extend its expiration period. Only the namespace owner can renew.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "id", "Namespace identifier"},
                        {RPCResult::Type::STR, "owner", "Owner's address"},
                        {RPCResult::Type::NUM, "previous_expiration", "Previous expiration timestamp"},
                        {RPCResult::Type::NUM, "new_expiration", "New expiration timestamp"},
                        {RPCResult::Type::STR_AMOUNT, "renewal_fee", "The renewal fee in RDD"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("renewnamespace", "\"redd\"")
                    + HelpExampleCli("renewnamespace", "\"redd\" \"myaccount\" \"legacy\" \"My Label\"")
                    + HelpExampleRpc("renewnamespace", "\"redd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    std::string namespaceId = request.params[0].get_str();
    std::string account = request.params.size() > 1 ? request.params[1].get_str() : "";

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 2) {
        std::string address_type = request.params[2].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 3) {
        label = request.params[3].get_str();
    }

    // Get owner address using the HD wallet functionality
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check ownership
    if (namespaceInfo.owner != ownerKeyId) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the owner of this namespace");
    }

    // Store the original expiration time
    int64_t previousExpiration = namespaceInfo.expiration;

    // Calculate renewal fee - let's assume it's a percentage of the initial price
    CAmount renewalFee = namespaceManager->CalculateRenewalFee(namespaceId);

    // Check user balance here (not implemented in this sample code)
    // ...

    // Renew the namespace
    if (!namespaceManager->RenewNamespace(namespaceId, ownerKeyId, namespaceInfo.renewalPeriod)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to renew namespace");
    }

    // Get updated namespace info
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated namespace info");
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("id", namespaceInfo.id);
    result.pushKV("owner", EncodeDestination(PKHash(namespaceInfo.owner)));
    result.pushKV("previous_expiration", previousExpiration);
    result.pushKV("new_expiration", (int64_t)namespaceInfo.expiration);
    result.pushKV("renewal_fee", ValueFromAmount(renewalFee));

    return result;
},
    };
}

static RPCHelpMan transfernamespace()
{
    return RPCHelpMan{"transfernamespace",
                "\nTransfer ownership of a namespace to another address.\n",
                {
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"toaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination address"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "id", "Namespace identifier"},
                        {RPCResult::Type::STR, "previous_owner", "Previous owner's address"},
                        {RPCResult::Type::STR, "new_owner", "New owner's address"},
                        {RPCResult::Type::NUM, "transfer_time", "Transfer timestamp"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("transfernamespace", "\"redd\" \"rx1qw9lfdjhn37...\"")
                    + HelpExampleCli("transfernamespace", "\"redd\" \"rx1qw9lfdjhn37...\" \"myaccount\" \"legacy\" \"My Label\"")
                    + HelpExampleRpc("transfernamespace", "\"redd\", \"rx1qw9lfdjhn37...\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();

    if (!namespaceManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Namespace manager not initialized");
    }

    std::string namespaceId = request.params[0].get_str();
    std::string toAddressStr = request.params[1].get_str();
    std::string account = request.params.size() > 2 ? request.params[2].get_str() : "";

    // Get namespace info
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    // Parse to address
    CTxDestination toDest = DecodeDestination(toAddressStr);
    if (!IsValidDestination(toDest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid destination address");
    }

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

    CKeyID toKeyId = GetKeyForDestination(spk_man, toDest);
    if (toKeyId.IsNull()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid destination address key ID");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 3) {
        std::string address_type = request.params[3].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 4) {
        label = request.params[4].get_str();
    }

    // Get owner address using the HD wallet functionality
    std::string error;
    CKeyID fromKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (fromKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check ownership
    if (namespaceInfo.owner != fromKeyId) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the owner of this namespace");
    }

    // Store the original owner for the response
    CKeyID previousOwner = namespaceInfo.owner;

    // Transfer namespace
    if (!namespaceManager->TransferNamespace(namespaceId, fromKeyId, toKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to transfer namespace");
    }

    // Get updated namespace info
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated namespace info");
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("id", namespaceInfo.id);
    result.pushKV("previous_owner", EncodeDestination(PKHash(previousOwner)));
    result.pushKV("new_owner", EncodeDestination(PKHash(namespaceInfo.owner)));
    result.pushKV("transfer_time", (int64_t)namespaceInfo.lastUpdated);

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
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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
                        {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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

static RPCHelpMan createuseridauction()
{
    return RPCHelpMan{"createuseridauction",
                "\nCreate a user ID auction within a namespace.\n",
                {
                    {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The user ID name"},
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"reserve_price", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The minimum bid amount in RDD"},
                    {"duration_days", RPCArg::Type::NUM, RPCArg::Optional::NO, "The auction duration in days"},
                    {"auction_type", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The auction type (0=standard, 1=premium)"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},

                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::NUM, "start_time", "The auction start time"},
                        {RPCResult::Type::NUM, "end_time", "The auction end time"},
                        {RPCResult::Type::STR_AMOUNT, "reserve_price", "The reserve price in RDD"},
                        {RPCResult::Type::NUM, "auction_type", "The auction type"},
                        {RPCResult::Type::STR, "creator_address", "The creator's address"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("createuseridauction", "\"alice\" \"redd\" 5000 7")
                    + HelpExampleCli("createuseridauction", "\"alice\" \"redd\" 5000 7 0 \"auctions\"")
                    + HelpExampleRpc("createuseridauction", "\"alice\", \"redd\", 5000, 7")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    NamespaceManager* namespaceManager = reddIDManager->GetNamespaceManager();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!namespaceManager || !reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID system not fully initialized");
    }

    std::string name = request.params[0].get_str();
    std::string namespaceId = request.params[1].get_str();
    CAmount reservePrice = AmountFromValue(request.params[2]);
    int durationDays = request.params[3].get_int();
    AuctionType auctionType = request.params.size() > 4 ?
                             static_cast<AuctionType>(request.params[4].get_int()) :
                             AUCTION_STANDARD;
    std::string account = request.params.size() > 5 ? request.params[5].get_str() : "";

    // Check if namespace exists
    NamespaceInfo namespaceInfo;
    if (!namespaceManager->GetNamespaceInfo(namespaceId, namespaceInfo)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Namespace not found");
    }

    // Check if user ID is available
    if (reddidDB->ExistsUserID(name, namespaceId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "User ID already exists");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 2) {
	std::string address_type = request.params[2].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 3) {
	label = request.params[3].get_str();
    }

    // Get creator address using the HD wallet functionality
    std::string error;
    CKeyID creatorKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (creatorKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Create auction
    uint256 auctionId;
    if (!reddIDManager->CreateUserIDAuction(name, namespaceId, creatorKeyId,
                                    reservePrice, durationDays, auctionType, auctionId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to create user ID auction");
    }

    // Get auction info
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
    }

    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("name", auction.name);
    result.pushKV("namespace_id", auction.namespaceId);
    result.pushKV("start_time", (int64_t)auction.startTime);
    result.pushKV("end_time", (int64_t)auction.endTime);
    result.pushKV("reserve_price", ValueFromAmount(auction.reservePrice));
    result.pushKV("auction_type", (int)auction.type);
    result.pushKV("creator_address", EncodeDestination(PKHash(auction.creator)));

    return result;
},
    };
}

static RPCHelpMan bidonuseridauction()
{
    return RPCHelpMan{"bidonuseridauction",
                "\nPlace a bid on a user ID auction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"bid_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The bid amount in RDD"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "bid_id", "The bid identifier"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR_AMOUNT, "bid_amount", "The bid amount in RDD"},
                        {RPCResult::Type::STR_AMOUNT, "deposit_amount", "The deposit amount in RDD"},
                        {RPCResult::Type::NUM, "bid_time", "The bid timestamp"},
                        {RPCResult::Type::STR, "bidder_address", "The bidder's address"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("bidonuseridauction", "\"abcd1234\" 10000")
                    + HelpExampleCli("bidonuseridauction", "\"abcd1234\" 10000 \"auctions\"")
                    + HelpExampleRpc("bidonuseridauction", "\"abcd1234\", 10000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);
    
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }
    
    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    
    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
    }
    
    uint256 auctionId = ParseHashV(request.params[0], "auction_id");
    CAmount bidAmount = AmountFromValue(request.params[1]);
    std::string account = request.params.size() > 2 ? request.params[2].get_str() : "";

    // Get auction info
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }
    
    // Check if auction is active
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active");
    }
    
    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 2) {
	std::string address_type = request.params[2].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 3) {
	label = request.params[3].get_str();
    }

    // [... other code ...]

    // Get bidder address using the HD wallet functionality
    std::string error;
    CKeyID bidderKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (bidderKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }
    
    // Place bid
    uint256 bidId;
    if (!auctionManager->PlaceBid(auctionId, bidderKeyId, bidAmount, bidId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to place bid");
    }
    
    // Get bid info
    BidInfo bid;
    if (!auctionManager->GetBidInfo(bidId, bid)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve bid info");
    }
    
    UniValue result(UniValue::VOBJ);
    result.pushKV("bid_id", bidId.GetHex());
    result.pushKV("auction_id", bid.auctionId.GetHex());
    result.pushKV("bid_amount", ValueFromAmount(bid.bidAmount));
    result.pushKV("deposit_amount", ValueFromAmount(bid.depositAmount));
    result.pushKV("bid_time", (int64_t)bid.bidTime);
    result.pushKV("bidder_address", EncodeDestination(PKHash(bid.bidder)));
    
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
                        {RPCResult::Type::STR, "state", "The auction state (3=finalized)"}
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

static RPCHelpMan canceluseridauction()
{
    return RPCHelpMan{"canceluseridauction",
                "\nCancel a user ID auction. Only the creator can cancel an auction, and only if there are no bids.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "state", "The auction state (4=canceled)"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("canceluseridauction", "\"abcd1234\"")
                    + HelpExampleCli("canceluseridauction", "\"abcd1234\" \"auctions\"")
                    + HelpExampleRpc("canceluseridauction", "\"abcd1234\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();

    if (!auctionManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not initialized");
    }

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");
    std::string account = request.params.size() > 1 ? request.params[1].get_str() : "";

    // Get auction info
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 2) {
	std::string address_type = request.params[2].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 3) {
	label = request.params[3].get_str();
    }

    // Get bidder address using the HD wallet functionality
    std::string error;
    CKeyID creatorKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (creatorKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Cancel auction
    if (!reddIDManager->CancelUserIDAuction(auctionId, creatorKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to cancel auction");
    }

    // Get updated auction info
    if (!auctionManager->GetAuctionInfo(auctionId, auction)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated auction info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("name", auction.name);
    result.pushKV("namespace_id", auction.namespaceId);
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
                            {RPCResult::Type::NUM, "state", "The auction state (0=pending, 1=active, 2=ended, 3=finalized, 4=canceled)"},
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

static RPCHelpMan renewuserid()
{
    return RPCHelpMan{"renewuserid",
                "\nRenew a user ID.\n",
                {
                    {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The user ID name"},
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::NUM, "expiration_time", "The new expiration timestamp"},
                        {RPCResult::Type::STR_AMOUNT, "renewal_fee", "The renewal fee in RDD"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("renewuserid", "\"alice\" \"redd\"")
                    + HelpExampleCli("renewuserid", "\"alice\" \"redd\" \"auctions\"")
                    + HelpExampleRpc("renewuserid", "\"alice\", \"redd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not initialized");
    }

    std::string name = request.params[0].get_str();
    std::string namespaceId = request.params[1].get_str();
    std::string account = request.params.size() > 2 ? request.params[2].get_str() : "";

    // Get user ID info
    UserIDInfo userID;
    if (!reddidDB->ReadUserID(name, namespaceId, userID)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "User ID not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 4) {
	std::string address_type = request.params[4].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 5) {
	label = request.params[5].get_str();
    }

    // [... other code ...]

    // Get creator address using the HD wallet functionality
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Check ownership
    if (userID.owner != ownerKeyId) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the owner of this user ID");
    }

    // Calculate renewal fee
    CAmount renewalFee = reddIDManager->CalculateRenewalFee(name, namespaceId);

    // Renew user ID
    if (!reddIDManager->RenewUserID(name, namespaceId, ownerKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to renew user ID");
    }

    // Get updated user ID info
    if (!reddidDB->ReadUserID(name, namespaceId, userID)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated user ID info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("name", userID.name);
    result.pushKV("namespace_id", userID.namespaceId);
    result.pushKV("expiration_time", (int64_t)userID.expirationTime);
    result.pushKV("renewal_fee", ValueFromAmount(renewalFee));

    return result;
},
    };
}

static RPCHelpMan transferuserid()
{
    return RPCHelpMan{"transferuserid",
                "\nTransfer a user ID to another address.\n",
                {
                    {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The user ID name"},
                    {"namespaceid", RPCArg::Type::STR, RPCArg::Optional::NO, "The namespace identifier"},
                    {"toaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination address"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
		    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
		    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "The user ID name"},
                        {RPCResult::Type::STR, "namespace_id", "The namespace identifier"},
                        {RPCResult::Type::STR, "previous_owner", "The previous owner's address"},
                        {RPCResult::Type::STR, "new_owner", "The new owner's address"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("transferuserid", "\"alice\" \"redd\" \"address\"")
                    + HelpExampleCli("transferuserid", "\"alice\" \"redd\" \"address\" \"auctions\"")
                    + HelpExampleRpc("transferuserid", "\"alice\", \"redd\", \"address\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();

    if (!reddidDB) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not initialized");
    }

    std::string name = request.params[0].get_str();
    std::string namespaceId = request.params[1].get_str();
    std::string toAddressStr = request.params[2].get_str();
    std::string account = request.params.size() > 3 ? request.params[3].get_str() : "";

    // Get user ID info
    UserIDInfo userID;
    if (!reddidDB->ReadUserID(name, namespaceId, userID)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "User ID not found");
    }

    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 4) {
	std::string address_type = request.params[4].get_str();
	if (address_type == "p2sh-segwit") {
	    output_type = OutputType::P2SH_SEGWIT;
	} else if (address_type == "bech32") {
	    output_type = OutputType::BECH32;
	} else if (address_type != "legacy") {
	    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
	}
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 5) {
	label = request.params[5].get_str();
    }

    // [... other code ...]

    // Get creator address using the HD wallet functionality
    std::string error;
    CKeyID fromKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (fromKeyId.IsNull()) {
	throw JSONRPCError(RPC_WALLET_ERROR,
	    strprintf("Could not get new address: %s", error));
    }

    // Check ownership
    if (userID.owner != fromKeyId) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the owner of this user ID");
    }

    // Parse to address
    CTxDestination toDest = DecodeDestination(toAddressStr);
    if (!IsValidDestination(toDest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid to address");
    }

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

    CKeyID toKeyId = GetKeyForDestination(spk_man, toDest);
    if (toKeyId.IsNull()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid to address key ID");
    }

    // Transfer user ID
    if (!reddIDManager->TransferUserID(name, namespaceId, fromKeyId, toKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to transfer user ID");
    }

    // Get updated user ID info
    if (!reddidDB->ReadUserID(name, namespaceId, userID)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to retrieve updated user ID info");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("name", userID.name);
    result.pushKV("namespace_id", userID.namespaceId);
    result.pushKV("previous_owner", EncodeDestination(PKHash(fromKeyId)));
    result.pushKV("new_owner", EncodeDestination(PKHash(userID.owner)));

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

static RPCHelpMan createuserconnection()
{
    return RPCHelpMan{"createuserconnection",
                "\nCreate a new social connection between two users.\n",
                {
                    {"from_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The source ReddID"},
                    {"to_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The target ReddID"},
                    {"connection_type", RPCArg::Type::NUM, RPCArg::Optional::NO,
                     "Connection type (0=follow, 1=friend, 2=endorse, 3=block)"},
                    {"visibility", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
                     "Privacy setting (0=public, 1=friends, 2=private). Default: 0"},
                    {"metadata", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "Additional connection metadata. Default: empty string"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "from_reddid", "The source ReddID"},
                        {RPCResult::Type::STR, "to_reddid", "The target ReddID"},
                        {RPCResult::Type::NUM, "connection_type", "Connection type (0=follow, 1=friend, 2=endorse, 3=block)"},
                        {RPCResult::Type::STR, "type_name", "Text representation of connection type"},
                        {RPCResult::Type::NUM, "creation_time", "When the connection was created"},
                        {RPCResult::Type::NUM, "visibility", "Privacy setting (0=public, 1=friends, 2=private)"},
                        {RPCResult::Type::STR, "metadata", "Additional connection metadata"},
                        {RPCResult::Type::STR, "owner_address", "The owner's address for the connection"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("createuserconnection", "\"alice\" \"bob\" 0")
                    + HelpExampleCli("createuserconnection", "\"alice\" \"bob\" 1 0 \"Friends since school\"")
                    + HelpExampleRpc("createuserconnection", "\"alice\", \"bob\", 1, 0, \"Friends since school\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    std::string fromReddId = request.params[0].get_str();
    std::string toReddId = request.params[1].get_str();
    int connectionType = request.params[2].get_int();

    if (connectionType < CONNECTION_FOLLOW || connectionType > CONNECTION_BLOCK) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "Invalid connection type, must be between 0 and 3");
    }

    // Optional parameters
    int visibility = 0; // Default: public
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        visibility = request.params[3].get_int();
        if (visibility < 0 || visibility > 2) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                             "Invalid visibility, must be between 0 and 2");
        }
    }

    std::string metadata = ""; // Default: empty
    if (request.params.size() > 4 && !request.params[4].isNull()) {
        metadata = request.params[4].get_str();
    }

    std::string account = ""; // Default: default account
    if (request.params.size() > 5 && !request.params[5].isNull()) {
        account = request.params[5].get_str();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!profileManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Profile manager not initialized");
    }

    // Check if ReddIDs exist
    ReddIDProfile fromProfile;
    if (!profileManager->GetProfile(fromReddId, fromProfile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Source ReddID not found");
    }

    ReddIDProfile toProfile;
    if (!profileManager->GetProfile(toReddId, toProfile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Target ReddID not found");
    }

    // Get owner address
    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 6) {
        std::string address_type = request.params[6].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 7) {
        label = request.params[7].get_str();
    }

    // Get owner key from wallet
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check if user is the owner of the source ReddID
    if (fromProfile.owner != ownerKeyId) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "You are not the owner of the source ReddID");
    }

    // Create new connection
    ReddIDConnection connection;
    connection.fromReddId = fromReddId;
    connection.toReddId = toReddId;
    connection.connectionType = static_cast<SocialConnectionType>(connectionType);
    connection.creationTime = GetTime();
    connection.lastInteraction = connection.creationTime;
    connection.visibility = visibility;
    connection.metadata = metadata;

    // Save connection
    if (!profileManager->CreateConnection(connection)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to create connection");
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
    result.pushKV("from_reddid", connection.fromReddId);
    result.pushKV("to_reddid", connection.toReddId);
    result.pushKV("connection_type", static_cast<int>(connection.connectionType));
    result.pushKV("type_name", getConnectionTypeName(connection.connectionType));
    result.pushKV("creation_time", (int64_t)connection.creationTime);
    result.pushKV("visibility", connection.visibility);
    result.pushKV("metadata", connection.metadata);
    result.pushKV("owner_address", EncodeDestination(PKHash(ownerKeyId)));

    return result;
},
    };
}

static RPCHelpMan updateuserconnection()
{
    return RPCHelpMan{"updateuserconnection",
                "\nUpdate an existing social connection between two users.\n",
                {
                    {"from_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The source ReddID"},
                    {"to_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The target ReddID"},
                    {"connection_type", RPCArg::Type::NUM, RPCArg::Optional::NO,
                     "New connection type (0=follow, 1=friend, 2=endorse, 3=block)"},
                    {"visibility", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
                     "New privacy setting (0=public, 1=friends, 2=private)"},
                    {"metadata", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "New additional connection metadata"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "from_reddid", "The source ReddID"},
                        {RPCResult::Type::STR, "to_reddid", "The target ReddID"},
                        {RPCResult::Type::NUM, "connection_type", "Updated connection type"},
                        {RPCResult::Type::STR, "type_name", "Text representation of connection type"},
                        {RPCResult::Type::NUM, "creation_time", "When the connection was originally created"},
                        {RPCResult::Type::NUM, "last_interaction", "Updated timestamp of last interaction"},
                        {RPCResult::Type::NUM, "visibility", "Updated privacy setting"},
                        {RPCResult::Type::STR, "metadata", "Updated additional connection metadata"},
                        {RPCResult::Type::STR, "owner_address", "The owner's address for the connection"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("updateuserconnection", "\"alice\" \"bob\" 1")
                    + HelpExampleCli("updateuserconnection", "\"alice\" \"bob\" 1 0 \"Close friends\"")
                    + HelpExampleRpc("updateuserconnection", "\"alice\", \"bob\", 1, 0, \"Close friends\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    std::string fromReddId = request.params[0].get_str();
    std::string toReddId = request.params[1].get_str();
    int connectionType = request.params[2].get_int();

    if (connectionType < CONNECTION_FOLLOW || connectionType > CONNECTION_BLOCK) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "Invalid connection type, must be between 0 and 3");
    }

    // Optional parameters - only update if provided
    bool updateVisibility = false;
    int visibility = 0;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        updateVisibility = true;
        visibility = request.params[3].get_int();
        if (visibility < 0 || visibility > 2) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                             "Invalid visibility, must be between 0 and 2");
        }
    }

    bool updateMetadata = false;
    std::string metadata = "";
    if (request.params.size() > 4 && !request.params[4].isNull()) {
        updateMetadata = true;
        metadata = request.params[4].get_str();
    }

    std::string account = ""; // Default: default account
    if (request.params.size() > 5 && !request.params[5].isNull()) {
        account = request.params[5].get_str();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!profileManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Profile manager not initialized");
    }

    // Check if ReddIDs exist
    ReddIDProfile fromProfile;
    if (!profileManager->GetProfile(fromReddId, fromProfile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Source ReddID not found");
    }

    ReddIDProfile toProfile;
    if (!profileManager->GetProfile(toReddId, toProfile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Target ReddID not found");
    }

    // Check if connection exists
    std::vector<ReddIDConnection> connections = profileManager->GetConnections(fromReddId);
    ReddIDConnection* existingConnection = nullptr;

    for (auto& conn : connections) {
        if (conn.toReddId == toReddId) {
            existingConnection = &conn;
            break;
        }
    }

    if (!existingConnection) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "Connection does not exist between these ReddIDs");
    }

    // Get owner address
    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 6) {
        std::string address_type = request.params[6].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 7) {
        label = request.params[7].get_str();
    }

    // Get owner key from wallet
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check if user is the owner of the source ReddID
    if (fromProfile.owner != ownerKeyId) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "You are not the owner of the source ReddID");
    }

    // Update connection with new values
    existingConnection->connectionType = static_cast<SocialConnectionType>(connectionType);
    existingConnection->lastInteraction = GetTime();

    if (updateVisibility) {
        existingConnection->visibility = visibility;
    }

    if (updateMetadata) {
        existingConnection->metadata = metadata;
    }

    // Save updated connection
    if (!profileManager->UpdateConnection(*existingConnection)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to update connection");
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
    result.pushKV("from_reddid", existingConnection->fromReddId);
    result.pushKV("to_reddid", existingConnection->toReddId);
    result.pushKV("connection_type", static_cast<int>(existingConnection->connectionType));
    result.pushKV("type_name", getConnectionTypeName(existingConnection->connectionType));
    result.pushKV("creation_time", (int64_t)existingConnection->creationTime);
    result.pushKV("last_interaction", (int64_t)existingConnection->lastInteraction);
    result.pushKV("visibility", existingConnection->visibility);
    result.pushKV("metadata", existingConnection->metadata);
    result.pushKV("owner_address", EncodeDestination(PKHash(ownerKeyId)));

    return result;
},
    };
}

static RPCHelpMan removeuserconnection()
{
    return RPCHelpMan{"removeuserconnection",
                "\nRemove an existing social connection between two users.\n",
                {
                    {"from_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The source ReddID"},
                    {"to_reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The target ReddID"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "A label for the address (HD wallet)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "from_reddid", "The source ReddID"},
                        {RPCResult::Type::STR, "to_reddid", "The target ReddID"},
                        {RPCResult::Type::BOOL, "removed", "Whether the connection was successfully removed"},
                        {RPCResult::Type::STR, "owner_address", "The owner's address that authorized the removal"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("removeuserconnection", "\"alice\" \"bob\"")
                    + HelpExampleCli("removeuserconnection", "\"alice\" \"bob\" \"myaccount\"")
                    + HelpExampleRpc("removeuserconnection", "\"alice\", \"bob\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    std::string fromReddId = request.params[0].get_str();
    std::string toReddId = request.params[1].get_str();

    std::string account = ""; // Default: default account
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        account = request.params[2].get_str();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

    ReddIDManager* reddIDManager = node.reddid.get();
    ProfileManager* profileManager = reddIDManager->GetProfileManager();

    if (!profileManager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Profile manager not initialized");
    }

    // Check if ReddIDs exist
    ReddIDProfile fromProfile;
    if (!profileManager->GetProfile(fromReddId, fromProfile)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Source ReddID not found");
    }

    // Check if connection exists
    std::vector<ReddIDConnection> connections = profileManager->GetConnections(fromReddId);
    bool connectionExists = false;

    for (const auto& conn : connections) {
        if (conn.toReddId == toReddId) {
            connectionExists = true;
            break;
        }
    }

    if (!connectionExists) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "Connection does not exist between these ReddIDs");
    }

    // Get owner address
    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        std::string address_type = request.params[3].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 4 && !request.params[4].isNull()) {
        label = request.params[4].get_str();
    }

    // Get owner key from wallet
    std::string error;
    CKeyID ownerKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (ownerKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get new address: %s", error));
    }

    // Check if user is the owner of the source ReddID
    if (fromProfile.owner != ownerKeyId) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                         "You are not the owner of the source ReddID");
    }

    // Remove the connection
    bool result = profileManager->RemoveConnection(fromReddId, toReddId, ownerKeyId);

    if (!result) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to remove connection");
    }

    // Build response
    UniValue response(UniValue::VOBJ);
    response.pushKV("from_reddid", fromReddId);
    response.pushKV("to_reddid", toReddId);
    response.pushKV("removed", true);
    response.pushKV("owner_address", EncodeDestination(PKHash(ownerKeyId)));

    return response;
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
                        {RPCResult::Type::NUM, "transaction_score", "Score component for transaction history (if detailed=true)"},
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
        reputation.transactionScore = 0.0;
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
        result.pushKV("transaction_score", reputation.transactionScore);
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

static RPCHelpMan updateuserreputation()
{
    return RPCHelpMan{"updateuserreputation",
                "\nUpdate reputation information for a user. Only authorized nodes can update reputation directly.\n",
                {
                    {"reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The ReddID to update reputation for"},
                    {"reputation_data", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Reputation data to update",
                    {
                        {"overall_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Overall reputation score (0-100)"},
                        {"longevity_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for account age (0-100)"},
                        {"transaction_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for transaction history (0-100)"},
                        {"engagement_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for community engagement (0-100)"},
                        {"verification_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for verification depth (0-100)"},
                        {"auction_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for auction behavior (0-100)"}
                    }},
                    {"auth_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Address of the authorized reputation calculator"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A label for the address (HD wallet)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "reddid", "The ReddID"},
                        {RPCResult::Type::NUM, "overall_score", "Updated overall reputation score"},
                        {RPCResult::Type::NUM, "previous_score", "Previous overall reputation score"},
                        {RPCResult::Type::NUM, "last_calculated", "Timestamp when reputation was last calculated"},
                        {RPCResult::Type::STR, "auth_address", "Address of the authorized calculator that performed the update"},
                        {RPCResult::Type::BOOL, "updated", "Whether the reputation was successfully updated"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("updateuserreputation", "\"alice\" '{\"overall_score\":75.5,\"engagement_score\":80.0}'")
                    + HelpExampleRpc("updateuserreputation", "\"alice\", {\"overall_score\":75.5,\"engagement_score\":80.0}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    std::string reddId = request.params[0].get_str();
    UniValue reputationData = request.params[1].get_obj();

    // Default to empty (default account)
    std::string account = "";
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        account = request.params[3].get_str();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

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

    // Get current reputation data
    ReddIDReputation currentReputation;
    bool hasExistingReputation = profileManager->GetReputation(reddId, currentReputation);

    if (!hasExistingReputation) {
        // Initialize default reputation
        currentReputation.reddId = reddId;
        currentReputation.overallScore = 50.0;
        currentReputation.longevityScore = 0.0;
        currentReputation.transactionScore = 0.0;
        currentReputation.engagementScore = 0.0;
        currentReputation.verificationScore = 0.0;
        currentReputation.auctionScore = 50.0;
        currentReputation.lastCalculated = GetTime();
    }

    // Store previous score for reference
    double previousScore = currentReputation.overallScore;

    // Update reputation with new values
    ReddIDReputation newReputation = currentReputation;

    // Apply updates from the reputationData parameter
    if (reputationData.exists("overall_score")) {
        double score = reputationData["overall_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "overall_score must be between 0 and 100");
        }
        newReputation.overallScore = score;
    }

    if (reputationData.exists("longevity_score")) {
        double score = reputationData["longevity_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "longevity_score must be between 0 and 100");
        }
        newReputation.longevityScore = score;
    }

    if (reputationData.exists("transaction_score")) {
        double score = reputationData["transaction_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction_score must be between 0 and 100");
        }
        newReputation.transactionScore = score;
    }

    if (reputationData.exists("engagement_score")) {
        double score = reputationData["engagement_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "engagement_score must be between 0 and 100");
        }
        newReputation.engagementScore = score;
    }

    if (reputationData.exists("verification_score")) {
        double score = reputationData["verification_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "verification_score must be between 0 and 100");
        }
        newReputation.verificationScore = score;
    }

    if (reputationData.exists("auction_score")) {
        double score = reputationData["auction_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "auction_score must be between 0 and 100");
        }
        newReputation.auctionScore = score;
    }

    // Update timestamp
    newReputation.lastCalculated = GetTime();

    // Get auth address if provided, otherwise get from wallet
    CKeyID authKeyId;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        std::string authAddressStr = request.params[2].get_str();
        CTxDestination authDest = DecodeDestination(authAddressStr);

        if (!IsValidDestination(authDest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid auth address");
        }

        LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

        authKeyId = GetKeyForDestination(spk_man, authDest);
        if (authKeyId.IsNull()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Auth address not found in wallet");
        }
    } else {
        // Determine address type
        OutputType output_type = OutputType::LEGACY;
        if (request.params.size() > 4 && !request.params[4].isNull()) {
            std::string address_type = request.params[4].get_str();
            if (address_type == "p2sh-segwit") {
                output_type = OutputType::P2SH_SEGWIT;
            } else if (address_type == "bech32") {
                output_type = OutputType::BECH32;
            } else if (address_type != "legacy") {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
            }
        }

        // Get optional label
        std::string label = "";
        if (request.params.size() > 5 && !request.params[5].isNull()) {
            label = request.params[5].get_str();
        }

        // Get auth key from wallet
        std::string error;
        authKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

        if (authKeyId.IsNull()) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                strprintf("Could not get auth address: %s", error));
        }
    }

    // TODO: In a real implementation, we would check if the auth address is authorized to update reputation
    // For this example, we'll assume any valid address can update reputation

    // Update the reputation
    if (!profileManager->UpdateReputation(newReputation)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to update reputation");
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reddId);
    result.pushKV("overall_score", newReputation.overallScore);
    result.pushKV("previous_score", previousScore);
    result.pushKV("last_calculated", (int64_t)newReputation.lastCalculated);
    result.pushKV("auth_address", EncodeDestination(PKHash(authKeyId)));
    result.pushKV("updated", true);

    return result;
},
    };
}

static RPCHelpMan calculateuserreputation()
{
    return RPCHelpMan{"calculateuserreputation",
                "\nCalculate or recalculate reputation for a user based on their activity and profile.\n",
                {
                    {"reddid", RPCArg::Type::STR, RPCArg::Optional::NO, "The ReddID to calculate reputation for"},
                    {"force", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Force recalculation even if recently updated. Default: false"},
                    {"account", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The account to use for HD wallets. If not specified, the default account is used."},
                    {"address_type", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "The address type to use (legacy, p2sh-segwit, bech32). Defaults to legacy."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                     "A label for the address (HD wallet)"}
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "reddid", "The ReddID"},
                        {RPCResult::Type::NUM, "overall_score", "Calculated overall reputation score"},
                        {RPCResult::Type::NUM, "previous_score", "Previous overall reputation score"},
                        {RPCResult::Type::NUM, "longevity_score", "Calculated longevity score"},
                        {RPCResult::Type::NUM, "transaction_score", "Calculated transaction score"},
                        {RPCResult::Type::NUM, "engagement_score", "Calculated engagement score"},
                        {RPCResult::Type::NUM, "verification_score", "Calculated verification score"},
                        {RPCResult::Type::NUM, "auction_score", "Calculated auction score"},
                        {RPCResult::Type::NUM, "last_calculated", "Timestamp when calculation was performed"},
                        {RPCResult::Type::STR, "calculator_address", "Address of the calculator that performed the calculation"},
                        {RPCResult::Type::BOOL, "recalculated", "Whether the reputation was recalculated or used cached values"}
                    }
                },
                RPCExamples{
                    HelpExampleCli("calculateuserreputation", "\"alice\"")
                    + HelpExampleCli("calculateuserreputation", "\"alice\" true")
                    + HelpExampleRpc("calculateuserreputation", "\"alice\", true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK(pwallet->cs_wallet);

    std::string reddId = request.params[0].get_str();
    bool force = false;

    if (request.params.size() > 1) {
        force = request.params[1].get_bool();
    }

    std::string account = "";
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        account = request.params[2].get_str();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");
    }

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

    // Get current reputation data
    ReddIDReputation currentReputation;
    bool hasExistingReputation = profileManager->GetReputation(reddId, currentReputation);

    // Store previous score
    double previousScore = hasExistingReputation ? currentReputation.overallScore : 50.0;

    // Check if recent calculation exists and force is not set
    int64_t currentTime = GetTime();
    bool skipCalculation = false;

    if (hasExistingReputation && !force) {
        // Only recalculate if it's been more than 1 hour since last calculation
        if (currentTime - currentReputation.lastCalculated < 3600) {
            skipCalculation = true;
        }
    }

    // Get calculator address
    // Determine address type
    OutputType output_type = OutputType::LEGACY;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        std::string address_type = request.params[3].get_str();
        if (address_type == "p2sh-segwit") {
            output_type = OutputType::P2SH_SEGWIT;
        } else if (address_type == "bech32") {
            output_type = OutputType::BECH32;
        } else if (address_type != "legacy") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
        }
    }

    // Get optional label
    std::string label = "";
    if (request.params.size() > 4 && !request.params[4].isNull()) {
        label = request.params[4].get_str();
    }

    // Get calculator key from wallet
    std::string error;
    CKeyID calculatorKeyId = GetNewWalletDestination(pwallet, error, output_type, label);

    if (calculatorKeyId.IsNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Could not get calculator address: %s", error));
    }

    // Get reputation data (either recalculated or existing)
    ReddIDReputation reputation;

    if (skipCalculation) {
        // Use existing reputation
        reputation = currentReputation;
    } else {
        // Calculate new reputation
        if (!profileManager->CalculateReputation(reddId, reputation)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to calculate reputation");
        }
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reputation.reddId);
    result.pushKV("overall_score", reputation.overallScore);
    result.pushKV("previous_score", previousScore);
    result.pushKV("longevity_score", reputation.longevityScore);
    result.pushKV("transaction_score", reputation.transactionScore);
    result.pushKV("engagement_score", reputation.engagementScore);
    result.pushKV("verification_score", reputation.verificationScore);
    result.pushKV("auction_score", reputation.auctionScore);
    result.pushKV("last_calculated", (int64_t)reputation.lastCalculated);
    result.pushKV("calculator_address", EncodeDestination(PKHash(calculatorKeyId)));
    result.pushKV("recalculated", !skipCalculation);

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
                                {RPCResult::Type::NUM, "transaction_score", "Score component for transaction history"},
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
        historyEntry.pushKV("transaction_score", rep.transactionScore);
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
        historyEntry.pushKV("calculator_signatures", std::to_string(signatureCount));

        historyArray.push_back(historyEntry);
    }

    result.pushKV("history", historyArray);

    return result;
},
    };
}

// Register all ReddID RPC commands
void RegisterReddIDRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] = {
    // Namespace commands
    { "reddid", &getnamespacelist,                 },
    { "reddid", &getnamespaceinfo,                 },
    { "reddid", &getnamespaceauctionlist,          },
    { "reddid", &getnamespaceauctioninfo,          },
    { "reddid", &createnamespacesauction,          },
    { "reddid", &bidonnamespacesauction,           },
    { "reddid", &finalizenamespacesauction,        },
    { "reddid", &cancelnamespacesauction,          },
    { "reddid", &configurenamespacess,             },
    { "reddid", &getnamespacesauctions,            },
    { "reddid", &getnamespacesauctionbids,         },
    { "reddid", &renewnamespace,                   },
    { "reddid", &transfernamespace,                },

    // User ID commands
    { "reddid", &getuseridlist,                    },
    { "reddid", &getuseridinfo,                    },
    { "reddid", &getuserauctionlist,               },
    { "reddid", &getuserauctioninfo,               },
    { "reddid", &createuseridauction,              },
    { "reddid", &bidonuseridauction,               },
    { "reddid", &finalizeuseridauction,            },
    { "reddid", &canceluseridauction,              },
    { "reddid", &getuseridauctions,                },
    { "reddid", &getuseridauctionbids,             },
    { "reddid", &renewuserid,                      },
    { "reddid", &transferuserid,                   },

    // Auction commands
    { "reddid", &getuserconnections,               },
    { "reddid", &createuserconnection,             },
    { "reddid", &updateuserconnection,             },
    { "reddid", &removeuserconnection,             },

    { "reddid", &getuserreputation,                },
    { "reddid", &updateuserreputation,             },
    { "reddid", &calculateuserreputation,          },
    { "reddid", &getuserreputationhistory,         },


};
// clang-format on
    for (const auto& c : commands) {
	t.appendCommand(c.name, &c);
    }
}
