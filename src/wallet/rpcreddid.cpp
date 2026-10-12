// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <id/amounts.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/blockchain.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <sync.h>
#include <univalue.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <set>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

// The ReddID commands that use a wallet. This file is built only with a
// wallet, like wallet/init.cpp, which registers the commands. The commands
// that need no wallet are in rpc/reddid.cpp.
//
// The managers change their state from the chain tip, from the periodic task
// and from peer messages, always with cs_main held. Every command here takes
// cs_main, after the wallet lock, before it reads or changes that state: the
// wallet lock comes first because that is the order in which the wallet
// reaches cs_main when it funds or commits a transaction.

// Helper function to get a new destination from the wallet with the specified output type
static CKeyID GetNewWalletDestination(CWallet* pwallet, std::string& error, OutputType output_type = OutputType::LEGACY, const std::string& label = "") {
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

// The address type argument of a command, or an error if it names none
static OutputType ParseAddressType(const std::string& address_type)
{
    if (address_type == "p2sh-segwit") return OutputType::P2SH_SEGWIT;
    if (address_type == "bech32") return OutputType::BECH32;
    if (address_type != "legacy") throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown address type");
    return OutputType::LEGACY;
}

// Whether the wallet holds the private key of a key that owns a ReddID record:
// the creator of an auction, the owner of a namespace, of a user ID or of a
// profile. The commands that act for an owner ask this of the key in the
// record. They do not take a key from the wallet for the call: a new key is
// never the owner.
//
// A key that the wallet only watches does not count, and IsMine does not say
// which it is: a descriptor wallet without private keys reports every script
// that it knows as spendable. So the wallet is asked for the key itself. A
// legacy wallet says whether it has the key, locked or not. A descriptor
// wallet has to derive it, which it can only do when it is unlocked: a locked
// one is told to unlock.
static bool WalletHoldsKey(const CWallet& wallet, const CKeyID& keyId) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (keyId.IsNull() || wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) return false;

    const CScript script = GetScriptForDestination(PKHash(keyId));
    if (wallet.IsMine(script) != ISMINE_SPENDABLE) return false;

    if (const LegacyScriptPubKeyMan* legacy = wallet.GetLegacyScriptPubKeyMan()) {
        return legacy->HaveKey(keyId);
    }

    EnsureWalletIsUnlocked(wallet);
    SignatureData sigdata;
    for (ScriptPubKeyMan* spk_man : wallet.GetScriptPubKeyMans(script, sigdata)) {
        const DescriptorScriptPubKeyMan* descriptors = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        CKey key;
        if (descriptors && descriptors->GetKey(script, keyId, key)) return true;
    }
    return false;
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

    LOCK2(pwallet->cs_wallet, cs_main);

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
    if (request.params.size() > 5) {
        std::string address_type = request.params[5].get_str();
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
    if (request.params.size() > 6) {
        label = request.params[6].get_str();
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

    LOCK2(pwallet->cs_wallet, cs_main);

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
                        {RPCResult::Type::NUM, "state", "The auction state (8=canceled)"}
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

    LOCK2(pwallet->cs_wallet, cs_main);

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


    // The wallet must hold the key of the creator of the auction
    const CKeyID creatorKeyId = auction.creator;
    if (!WalletHoldsKey(*pwallet, creatorKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the creator of this auction");
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the namespace
    if (request.params.size() > 4) {
        ParseAddressType(request.params[4].get_str());
    }

    if (request.params.size() > 5) {
        request.params[5].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the namespace
    const CKeyID ownerKeyId = namespaceInfo.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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

    // Check if we have pricing tiers to update. They are all read before
    // anything is changed, so that a tier that is refused changes nothing.
    bool replaceTiers = false;
    std::vector<std::pair<int, CAmount>> newTiers;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        UniValue tiersArray = request.params[2].get_array();

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

            // The namespace manager stores no tier with a price of zero
            if (minPrice <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "min_price must be positive");
            }

            // and one tier for a length: a second one would replace the first
            for (const auto& tier : newTiers) {
                if (tier.first == minLength) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("More than one pricing tier with min_length %d", minLength));
                }
            }

            newTiers.emplace_back(minLength, minPrice);
        }

        replaceTiers = true;
        configUpdated = true;
    }

    // Update namespace if anything changed
    if (configUpdated) {
        namespaceInfo.lastUpdated = GetTime();

        if (!namespaceManager->UpdateNamespace(namespaceInfo)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to update namespace configuration");
        }
    }

    // The tiers that were given replace the tiers that there are. This is
    // not atomic: if the namespace manager fails at one step, which the
    // checks above leave only a database error to cause, the steps before it
    // stay done and the command reports the failure.
    if (replaceTiers) {
        for (const PricingTier& tier : namespaceManager->GetPricingTiers(namespaceId)) {
            if (!namespaceManager->RemovePricingTier(namespaceId, tier.minLength)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Failed to remove a pricing tier");
            }
        }

        for (const auto& tier : newTiers) {
            if (!namespaceManager->AddPricingTier(namespaceId, tier.first, tier.second)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Failed to add a pricing tier");
            }
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the namespace
    if (request.params.size() > 2) {
        ParseAddressType(request.params[2].get_str());
    }

    if (request.params.size() > 3) {
        request.params[3].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the namespace
    const CKeyID ownerKeyId = namespaceInfo.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the namespace
    if (request.params.size() > 3) {
        ParseAddressType(request.params[3].get_str());
    }

    if (request.params.size() > 4) {
        request.params[4].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the namespace
    const CKeyID fromKeyId = namespaceInfo.owner;
    if (!WalletHoldsKey(*pwallet, fromKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    LOCK2(pwallet->cs_wallet, cs_main);

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
                        {RPCResult::Type::NUM, "state", "The auction state (8=canceled)"}
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // creator of the auction
    if (request.params.size() > 2) {
        ParseAddressType(request.params[2].get_str());
    }

    if (request.params.size() > 3) {
        request.params[3].get_str();  // the label
    }

    // The wallet must hold the key of the creator of the auction
    const CKeyID creatorKeyId = auction.creator;
    if (!WalletHoldsKey(*pwallet, creatorKeyId)) {
        throw JSONRPCError(RPC_MISC_ERROR, "You are not the creator of this auction");
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the user ID
    if (request.params.size() > 3) {
        ParseAddressType(request.params[3].get_str());
    }

    if (request.params.size() > 4) {
        request.params[4].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the user ID
    const CKeyID ownerKeyId = userID.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the user ID
    if (request.params.size() > 4) {
        ParseAddressType(request.params[4].get_str());
    }

    if (request.params.size() > 5) {
        request.params[5].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the user ID
    const CKeyID fromKeyId = userID.owner;
    if (!WalletHoldsKey(*pwallet, fromKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the source ReddID
    if (request.params.size() > 6) {
        ParseAddressType(request.params[6].get_str());
    }

    if (request.params.size() > 7) {
        request.params[7].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the source ReddID
    const CKeyID ownerKeyId = fromProfile.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the source ReddID
    if (request.params.size() > 6) {
        ParseAddressType(request.params[6].get_str());
    }

    if (request.params.size() > 7) {
        request.params[7].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the source ReddID
    const CKeyID ownerKeyId = fromProfile.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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

    LOCK2(pwallet->cs_wallet, cs_main);

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

    // The address type is still checked, and it and the label are not used:
    // they were for a new key, and the command acts with the key of the
    // owner of the source ReddID
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        ParseAddressType(request.params[3].get_str());
    }

    if (request.params.size() > 4 && !request.params[4].isNull()) {
        request.params[4].get_str();  // the label
    }

    // The wallet must hold the key of the owner of the source ReddID
    const CKeyID ownerKeyId = fromProfile.owner;
    if (!WalletHoldsKey(*pwallet, ownerKeyId)) {
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
                        {"reliability_score", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Score component for deposit reliability (0-100)"},
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

    LOCK2(pwallet->cs_wallet, cs_main);

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
        currentReputation.reliabilityScore = 0.0;
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

    if (reputationData.exists("reliability_score")) {
        double score = reputationData["reliability_score"].get_real();
        if (score < 0.0 || score > 100.0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "reliability_score must be between 0 and 100");
        }
        newReputation.reliabilityScore = score;
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
                        {RPCResult::Type::NUM, "reliability_score", "Calculated reliability score"},
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

    LOCK2(pwallet->cs_wallet, cs_main);

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
        // Calculate new reputation (routed through ReddIDManager for auction stats)
        if (!reddIDManager->CalculateReputation(reddId, reputation)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Failed to calculate reputation");
        }
    }

    // Build response
    UniValue result(UniValue::VOBJ);
    result.pushKV("reddid", reputation.reddId);
    result.pushKV("overall_score", reputation.overallScore);
    result.pushKV("previous_score", previousScore);
    result.pushKV("longevity_score", reputation.longevityScore);
    result.pushKV("reliability_score", reputation.reliabilityScore);
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

static RPCHelpMan reddidplacebid()
{
    return RPCHelpMan{"reddidplacebid",
                "\nPlace a deposit-backed bid on an auction. Constructs and broadcasts a BID_DEPOSIT transaction.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                    {"bid_amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The bid amount in RDD"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR_AMOUNT, "bid_amount", "The bid amount in RDD"},
                        {RPCResult::Type::STR_AMOUNT, "deposit_amount", "The deposit amount in RDD"},
                        {RPCResult::Type::NUM, "deposit_vout", "The deposit output index"},
                        {RPCResult::Type::STR, "bidder_address", "The bidder's address"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidplacebid", "\"abcd1234...\" 10000")
                    + HelpExampleRpc("reddidplacebid", "\"abcd1234...\", 10000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK2(pwallet->cs_wallet, cs_main);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");
    CAmount bidAmount = AmountFromValue(request.params[1]);

    // Validate auction
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_PENDING)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active or pending");

    // Check minimum bid: the reserve price and, over a current bid, the
    // minimum increment, as the node checks them when a block connects the bid
    CAmount minBid = auction.reservePrice;
    if (auction.currentBid > 0)
        minBid = std::max(minBid, MinimumNextBidForAuction(reddIDManager->GetNamespaceManager(), auction));
    if (bidAmount < minBid)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("Bid amount %s is below minimum %s",
                      FormatMoney(bidAmount), FormatMoney(minBid)));

    // Calculate deposit
    AuctionConfig config;
    std::string configId = auction.namespaceId.empty() ? "namespace" : "userid";
    if (!auctionManager->GetAuctionConfig(configId, config))
        config = AuctionConfig::UserIDDefaults();
    CAmount depositAmount = PercentOfAmount(bidAmount, config.depositPercentage);
    if (depositAmount <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Deposit amount calculated to zero");

    // Get fresh wallet address for bidder
    std::string error;
    CKeyID bidderKeyId = GetNewWalletDestination(pwallet, error);
    if (bidderKeyId.IsNull())
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Could not get new address: %s", error));

    // Build transaction
    CMutableTransaction rawTx;
    uint8_t depositVout = 1;

    // vout[0]: OP_RETURN
    CScript opReturnScript = reddIDManager->CreateBidDepositOpReturn(auctionId, bidAmount, depositVout, bidderKeyId);
    if (opReturnScript.empty())
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to create OP_RETURN script");
    rawTx.vout.push_back(CTxOut(0, opReturnScript));

    // vout[1]: deposit amount to bidder's own address
    CScript depositScript = GetScriptForDestination(PKHash(bidderKeyId));
    rawTx.vout.push_back(CTxOut(depositAmount, depositScript));

    // Fund, sign, broadcast
    CAmount nFeeRet;
    int nChangePosInOut = -1;
    bilingual_str fundError;
    CCoinControl coinControl;
    std::set<int> setSubtractFeeFromOutputs;

    if (!pwallet->FundTransaction(rawTx, nFeeRet, nChangePosInOut, fundError,
                                   /*lockUnspents=*/true, setSubtractFeeFromOutputs, coinControl))
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Failed to fund transaction: %s", fundError.original));

    if (!pwallet->SignTransaction(rawTx))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign transaction");

    CTransactionRef tx = MakeTransactionRef(std::move(rawTx));
    mapValue_t mapValue;
    pwallet->CommitTransaction(tx, std::move(mapValue), {});

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("bid_amount", ValueFromAmount(bidAmount));
    result.pushKV("deposit_amount", ValueFromAmount(depositAmount));
    result.pushKV("deposit_vout", (int)depositVout);
    result.pushKV("bidder_address", EncodeDestination(PKHash(bidderKeyId)));
    return result;
},
    };
}

static RPCHelpMan reddidsettleauction()
{
    return RPCHelpMan{"reddidsettleauction",
                "\nSettle an auction by spending the winning deposit. Must be called by the auction winner.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR_HEX, "bid_id", "The winning bid identifier"},
                        {RPCResult::Type::STR_AMOUNT, "settlement_amount", "The settlement amount in RDD"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidsettleauction", "\"abcd1234...\"")
                    + HelpExampleRpc("reddidsettleauction", "\"abcd1234...\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK2(pwallet->cs_wallet, cs_main);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Validate auction is in SETTLEMENT state
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    if (auction.state != AUCTION_SETTLEMENT)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("Auction is not in settlement state (current state: %d)", (int)auction.state));

    // Find winning bid
    std::vector<BidInfo> bids = reddIDManager->GetAuctionBids(auctionId);
    BidInfo winningBid;
    bool found = false;
    for (const auto& bid : bids) {
        if (bid.isWinner) {
            winningBid = bid;
            found = true;
            break;
        }
    }
    if (!found)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No winning bid found for auction");

    // Verify winner's deposit key belongs to this wallet
    CTxDestination winnerDest = PKHash(winningBid.bidder);
    if (pwallet->IsMine(GetScriptForDestination(winnerDest)) == ISMINE_NO)
        throw JSONRPCError(RPC_WALLET_ERROR, "Winning bid deposit key does not belong to this wallet");

    // Build transaction
    CMutableTransaction rawTx;

    // vout[0]: OP_RETURN
    CScript opReturnScript = reddIDManager->CreateAuctionSettleOpReturn(auctionId, winningBid.bidId);
    if (opReturnScript.empty())
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to create OP_RETURN script");
    rawTx.vout.push_back(CTxOut(0, opReturnScript));

    // Force-include the deposit UTXO as input
    CCoinControl coinControl;
    coinControl.Select(winningBid.depositUtxo);

    CAmount nFeeRet;
    int nChangePosInOut = -1;
    bilingual_str fundError;
    std::set<int> setSubtractFeeFromOutputs;

    if (!pwallet->FundTransaction(rawTx, nFeeRet, nChangePosInOut, fundError,
                                   /*lockUnspents=*/true, setSubtractFeeFromOutputs, coinControl))
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Failed to fund transaction: %s", fundError.original));

    if (!pwallet->SignTransaction(rawTx))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign transaction");

    CTransactionRef tx = MakeTransactionRef(std::move(rawTx));
    mapValue_t mapValue;
    pwallet->CommitTransaction(tx, std::move(mapValue), {});

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("bid_id", winningBid.bidId.GetHex());
    result.pushKV("settlement_amount", ValueFromAmount(winningBid.depositAmount));
    return result;
},
    };
}

static RPCHelpMan reddidfinalizeauction()
{
    return RPCHelpMan{"reddidfinalizeauction",
                "\nFinalize an ended auction, triggering settlement or completion.\n",
                {
                    {"auction_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The auction identifier"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::STR_HEX, "auction_id", "The auction identifier"},
                        {RPCResult::Type::STR, "state", "The new auction state"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidfinalizeauction", "\"abcd1234...\"")
                    + HelpExampleRpc("reddidfinalizeauction", "\"abcd1234...\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK2(pwallet->cs_wallet, cs_main);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    ReddIDManager* reddIDManager = node.reddid.get();
    AuctionManager* auctionManager = reddIDManager->GetAuctionManager();
    if (!auctionManager)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Auction manager not available");

    uint256 auctionId = ParseHashV(request.params[0], "auction_id");

    // Validate auction state
    AuctionInfo auction;
    if (!auctionManager->GetAuctionInfo(auctionId, auction))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction not found");
    if (auction.state != AUCTION_ACTIVE && auction.state != AUCTION_ENDED)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Auction is not active or ended");

    // Check auction end time
    int64_t currentTime = GetTime();
    if (auction.endTime > currentTime)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("Auction has not ended yet, %d seconds remaining",
                      auction.endTime - currentTime));

    // Build OP_RETURN for finalize
    // Format: 'R' + OP_AUCTION_FINALIZE('"') + auctionId(32)
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_AUCTION_FINALIZE);
    data.insert(data.end(), auctionId.begin(), auctionId.end());

    CScript opReturnScript;
    opReturnScript << OP_RETURN << data;

    CMutableTransaction rawTx;
    rawTx.vout.push_back(CTxOut(0, opReturnScript));

    CAmount nFeeRet;
    int nChangePosInOut = -1;
    bilingual_str fundError;
    CCoinControl coinControl;
    std::set<int> setSubtractFeeFromOutputs;

    if (!pwallet->FundTransaction(rawTx, nFeeRet, nChangePosInOut, fundError,
                                   /*lockUnspents=*/true, setSubtractFeeFromOutputs, coinControl))
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Failed to fund transaction: %s", fundError.original));

    if (!pwallet->SignTransaction(rawTx))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign transaction");

    CTransactionRef tx = MakeTransactionRef(std::move(rawTx));
    mapValue_t mapValue;
    pwallet->CommitTransaction(tx, std::move(mapValue), {});

    // Determine new state description
    std::string stateStr;
    std::vector<BidInfo> bids = reddIDManager->GetAuctionBids(auctionId);
    bool hasBids = false;
    for (const auto& bid : bids) {
        if (!bid.refunded) { hasBids = true; break; }
    }
    stateStr = hasBids ? "settlement" : "finalized";

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("auction_id", auctionId.GetHex());
    result.pushKV("state", stateStr);
    return result;
},
    };
}

static RPCHelpMan reddidlistmydeposits()
{
    return RPCHelpMan{"reddidlistmydeposits",
                "\nList all deposit locks belonging to this wallet.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
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
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("reddidlistmydeposits", "")
                    + HelpExampleRpc("reddidlistmydeposits", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return NullUniValue;
    CWallet* const pwallet = wallet.get();

    LOCK2(pwallet->cs_wallet, cs_main);

    NodeContext& node = EnsureAnyNodeContext(request.context);
    if (!node.reddid)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID manager not initialized");

    ReddIDManager* reddIDManager = node.reddid.get();
    ReddIDDB* reddidDB = reddIDManager->GetReddIDDB();
    if (!reddidDB)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ReddID database not available");

    std::vector<DepositLock> allLocks;
    reddidDB->ListAllDepositLocks(allLocks);

    UniValue results(UniValue::VARR);
    for (const auto& lock : allLocks) {
        // Filter to only deposits owned by this wallet
        CTxDestination dest = PKHash(lock.bidder);
        if (pwallet->IsMine(GetScriptForDestination(dest)) == ISMINE_NO)
            continue;

        std::string stateStr;
        switch (lock.state) {
            case DEPOSIT_LOCKED_ACTIVE:  stateStr = "active";    break;
            case DEPOSIT_RELEASED_LOST:  stateStr = "released";  break;
            case DEPOSIT_CONSUMED_WON:   stateStr = "consumed";  break;
            case DEPOSIT_FORFEITED:      stateStr = "forfeited"; break;
            default:                     stateStr = "unknown";   break;
        }

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", lock.utxo.hash.GetHex());
        entry.pushKV("vout", (int)lock.utxo.n);
        entry.pushKV("auction_id", lock.auctionId.GetHex());
        entry.pushKV("bid_id", lock.bidId.GetHex());
        entry.pushKV("bidder", EncodeDestination(dest));
        entry.pushKV("amount", ValueFromAmount(lock.amount));
        entry.pushKV("lock_height", lock.lockHeight);
        entry.pushKV("release_height", lock.releaseHeight);
        entry.pushKV("state", stateStr);
        results.push_back(entry);
    }

    return results;
},
    };
}

// Register the ReddID RPC commands that use a wallet
void RegisterReddIDWalletRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] = {
    // Namespace commands
    { "reddid", &createnamespacesauction,          },
    { "reddid", &bidonnamespacesauction,           },
    { "reddid", &cancelnamespacesauction,          },
    { "reddid", &configurenamespacess,             },
    { "reddid", &renewnamespace,                   },
    { "reddid", &transfernamespace,                },

    // User ID commands
    { "reddid", &createuseridauction,              },
    { "reddid", &bidonuseridauction,               },
    { "reddid", &canceluseridauction,              },
    { "reddid", &renewuserid,                      },
    { "reddid", &transferuserid,                   },

    // Auction commands
    { "reddid", &createuserconnection,             },
    { "reddid", &updateuserconnection,             },
    { "reddid", &removeuserconnection,             },
    { "reddid", &updateuserreputation,             },
    { "reddid", &calculateuserreputation,          },

    // Deposit and settlement commands
    { "reddid", &reddidplacebid,                   },
    { "reddid", &reddidsettleauction,              },
    { "reddid", &reddidfinalizeauction,            },
    { "reddid", &reddidlistmydeposits,             },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
