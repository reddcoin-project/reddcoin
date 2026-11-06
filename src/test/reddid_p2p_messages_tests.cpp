// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/reddid.h>
#include <id/reddid_p2p.h>
#include <pubkey.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <version.h>

#include <cstdint>
#include <ios>
#include <set>
#include <string>
#include <vector>

// The wire format of the ReddID peer-to-peer messages. Every message class is
// pinned by a literal vector: a change to the order, the width or the encoding
// of a field makes a node unable to talk to the nodes that run the old code.

namespace {
const int64_t T = 1700000000;

uint256 Fill256(unsigned char byte) { return uint256(std::vector<unsigned char>(32, byte)); }
CKeyID FillKey(unsigned char byte) { return CKeyID(uint160(std::vector<unsigned char>(20, byte))); }

template <typename T>
std::string Hex(const T& obj)
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << obj;
    return HexStr(stream);
}

/** Decode the vector into a new object, require that nothing is left over and return its serialization. */
template <typename T>
T FromHex(const std::string& hex)
{
    CDataStream stream(ParseHex(hex), SER_NETWORK, PROTOCOL_VERSION);
    T obj;
    stream >> obj;
    BOOST_CHECK(stream.empty());
    return obj;
}

/** The vector is what the object serializes to, and an object read from the vector serializes to it again. */
template <typename T>
T CheckVector(const T& obj, const std::string& hex)
{
    BOOST_CHECK_EQUAL(Hex(obj), hex);
    T decoded = FromHex<T>(hex);
    BOOST_CHECK_EQUAL(Hex(decoded), hex);
    return decoded;
}

/** Every proper prefix of the vector fails to deserialize. */
template <typename T>
void CheckTruncated(const std::string& hex)
{
    const std::vector<unsigned char> bytes = ParseHex(hex);
    for (size_t size = 0; size < bytes.size(); ++size) {
        CDataStream stream(std::vector<unsigned char>(bytes.begin(), bytes.begin() + size), SER_NETWORK, PROTOCOL_VERSION);
        T obj;
        BOOST_CHECK_THROW(stream >> obj, std::ios_base::failure);
    }
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_p2p_messages_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(command_names)
{
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_AUCTION_ANNOUNCE), "nsaucann");
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_AUCTION_BID), "nsaucbid");
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_AUCTION_FINALIZE), "nsaucfin");
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_AUCTION_CANCEL), "nsauccan");
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_CONFIG_REQUEST), "nsconreq");
    BOOST_CHECK_EQUAL(std::string(MSG_NAMESPACE_CONFIG_RESPONSE), "nsconres");
    BOOST_CHECK_EQUAL(std::string(MSG_USERID_AUCTION_ANNOUNCE), "uidaucann");
    BOOST_CHECK_EQUAL(std::string(MSG_USERID_AUCTION_BID), "uidaucbid");
    BOOST_CHECK_EQUAL(std::string(MSG_USERID_AUCTION_FINALIZE), "uidaucfin");
    BOOST_CHECK_EQUAL(std::string(MSG_USERID_AUCTION_CANCEL), "uidauccan");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_AUCTION_ANNOUNCE), "ridaucann");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_AUCTION_BID), "ridaucbid");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_AUCTION_FINALIZE), "ridaucfin");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_PROFILE_UPDATE), "ridprofup");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_CONNECTION), "ridconn");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_PROFILE_REQUEST), "ridprofreq");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_PROFILE_RESPONSE), "ridprofres");
    BOOST_CHECK_EQUAL(std::string(MSG_REDDID_REPUTATION_UPDATE), "ridrep");

    // 18 distinct commands, each short enough for the 12 byte command field of
    // a message header, and each with one of the three prefixes by which
    // net_processing recognises a ReddID message
    const std::set<std::string> commands{
        MSG_NAMESPACE_AUCTION_ANNOUNCE, MSG_NAMESPACE_AUCTION_BID, MSG_NAMESPACE_AUCTION_FINALIZE,
        MSG_NAMESPACE_AUCTION_CANCEL, MSG_NAMESPACE_CONFIG_REQUEST, MSG_NAMESPACE_CONFIG_RESPONSE,
        MSG_USERID_AUCTION_ANNOUNCE, MSG_USERID_AUCTION_BID, MSG_USERID_AUCTION_FINALIZE,
        MSG_USERID_AUCTION_CANCEL, MSG_REDDID_AUCTION_ANNOUNCE, MSG_REDDID_AUCTION_BID,
        MSG_REDDID_AUCTION_FINALIZE, MSG_REDDID_PROFILE_UPDATE, MSG_REDDID_CONNECTION,
        MSG_REDDID_PROFILE_REQUEST, MSG_REDDID_PROFILE_RESPONSE, MSG_REDDID_REPUTATION_UPDATE};
    BOOST_CHECK_EQUAL(commands.size(), 18U);
    for (const std::string& command : commands) {
        BOOST_CHECK(command.size() <= 12);
        BOOST_CHECK(command.substr(0, 2) == "ns" || command.substr(0, 3) == "uid" || command.substr(0, 3) == "rid");
    }
}

BOOST_AUTO_TEST_CASE(limits)
{
    BOOST_CHECK_EQUAL(MAX_REDDID_MESSAGE_SIZE, 1U * 1024 * 1024);
    BOOST_CHECK_EQUAL(MAX_PRICING_TIERS, 100U);
    BOOST_CHECK_EQUAL(DEFAULT_MAX_MESSAGES_PER_MINUTE, 20);
    BOOST_CHECK_EQUAL(MAX_AUCTION_ANNOUNCES_PER_MINUTE, 10);
    BOOST_CHECK_EQUAL(MAX_AUCTION_BIDS_PER_MINUTE, 20);
    BOOST_CHECK_EQUAL(MAX_PROFILE_UPDATES_PER_MINUTE, 5);
    BOOST_CHECK_EQUAL(MAX_CONNECTIONS_PER_MINUTE, 15);
    BOOST_CHECK_EQUAL(RATE_LIMIT_WINDOW_SECONDS, 60);
}

BOOST_AUTO_TEST_CASE(message_base)
{
    CReddIDMessageBase msg;
    BOOST_CHECK_EQUAL(msg.timestamp, 0);
    msg.timestamp = T;
    const std::string hex =
        "00f1536500000000";
    CReddIDMessageBase decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.timestamp, T);
    CheckTruncated<CReddIDMessageBase>(hex);

    // The timestamp is a signed 64 bit number and is not checked on receipt
    msg.timestamp = -1;
    BOOST_CHECK_EQUAL(Hex(msg), "ffffffffffffffff");
}

BOOST_AUTO_TEST_CASE(namespace_auction_announce)
{
    CNamespaceAuctionAnnounce msg;
    msg.timestamp = T;
    msg.auctionId = Fill256(0xa1);
    msg.namespaceId = "redd";
    msg.creator = FillKey(0xc1);
    msg.startTime = T + 1;
    msg.endTime = T + 2;
    msg.reservePrice = 10000 * COIN;
    msg.type = AUCTION_PREMIUM;
    msg.configHash = Fill256(0xa2);
    const std::string hex =
        "00f1536500000000a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a10472656464c1c1c1"
        "c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c101f153650000000002f15365000000000010a5d4e800000001a2a2a2a2a2a2"
        "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2";
    CNamespaceAuctionAnnounce decoded = CheckVector(msg, hex);
    BOOST_CHECK(decoded.auctionId == msg.auctionId);
    BOOST_CHECK_EQUAL(decoded.namespaceId, "redd");
    BOOST_CHECK(decoded.creator == msg.creator);
    BOOST_CHECK_EQUAL(decoded.startTime, T + 1);
    BOOST_CHECK_EQUAL(decoded.endTime, T + 2);
    BOOST_CHECK_EQUAL(decoded.reservePrice, 10000 * COIN);
    BOOST_CHECK(decoded.type == AUCTION_PREMIUM);
    BOOST_CHECK(decoded.configHash == msg.configHash);
    CheckTruncated<CNamespaceAuctionAnnounce>(hex);
}

BOOST_AUTO_TEST_CASE(namespace_config)
{
    CNamespaceConfig msg;
    msg.timestamp = T;
    msg.config.id = "redd";
    msg.config.owner = FillKey(0xc1);
    msg.config.allowNumbers = true;
    msg.config.allowHyphens = false;
    msg.config.allowUnderscores = true;
    msg.config.minLength = 1;
    msg.config.maxLength = 2;
    msg.config.renewalPeriod = 3;
    msg.config.gracePeriod = 4;
    msg.config.namespaceRevenuePct = 5;
    msg.config.burnPct = 6;
    msg.config.nodePct = 7;
    msg.config.devPct = 8;
    msg.config.minAuctionDuration = 9;
    msg.config.maxAuctionDuration = 10;
    msg.config.minBidIncrement = 5.25;
    msg.config.configHash = Fill256(0xa1);
    msg.config.lastUpdated = T + 1;
    msg.config.expiration = T + 2;
    PricingTier tier;
    tier.namespaceId = "redd";
    tier.minLength = 1;
    tier.minPrice = 3 * COIN;
    msg.pricingTiers.push_back(tier);
    tier.minLength = 4;
    tier.minPrice = 1 * COIN;
    msg.pricingTiers.push_back(tier);
    const std::string hex =
        "00f15365000000000472656464c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1010001010000000200000003000000"
        "0400000005000000060000000700000008000000090000000a0000000d020000a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
        "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a101f153650000000002f15365000000000204726564640100000000a3e1110000"
        "000004726564640400000000e1f50500000000";
    CNamespaceConfig decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.config.id, "redd");
    BOOST_CHECK(decoded.config.owner == msg.config.owner);
    BOOST_CHECK(decoded.config.allowNumbers);
    BOOST_CHECK(!decoded.config.allowHyphens);
    BOOST_CHECK(decoded.config.allowUnderscores);
    BOOST_CHECK_EQUAL(decoded.config.minLength, 1);
    BOOST_CHECK_EQUAL(decoded.config.maxLength, 2);
    BOOST_CHECK_EQUAL(decoded.config.renewalPeriod, 3);
    BOOST_CHECK_EQUAL(decoded.config.gracePeriod, 4);
    BOOST_CHECK_EQUAL(decoded.config.namespaceRevenuePct, 5);
    BOOST_CHECK_EQUAL(decoded.config.burnPct, 6);
    BOOST_CHECK_EQUAL(decoded.config.nodePct, 7);
    BOOST_CHECK_EQUAL(decoded.config.devPct, 8);
    BOOST_CHECK_EQUAL(decoded.config.minAuctionDuration, 9);
    BOOST_CHECK_EQUAL(decoded.config.maxAuctionDuration, 10);
    // The increment travels as hundredths in a 32 bit integer
    BOOST_CHECK_EQUAL(decoded.config.minBidIncrement, 5.25);
    BOOST_CHECK(decoded.config.configHash == msg.config.configHash);
    BOOST_CHECK_EQUAL(decoded.config.lastUpdated, T + 1);
    BOOST_CHECK_EQUAL(decoded.config.expiration, T + 2);
    BOOST_REQUIRE_EQUAL(decoded.pricingTiers.size(), 2U);
    BOOST_CHECK_EQUAL(decoded.pricingTiers[0].namespaceId, "redd");
    BOOST_CHECK_EQUAL(decoded.pricingTiers[0].minLength, 1);
    BOOST_CHECK_EQUAL(decoded.pricingTiers[0].minPrice, 3 * COIN);
    BOOST_CHECK_EQUAL(decoded.pricingTiers[1].minLength, 4);
    BOOST_CHECK_EQUAL(decoded.pricingTiers[1].minPrice, 1 * COIN);
    CheckTruncated<CNamespaceConfig>(hex);
}

BOOST_AUTO_TEST_CASE(auction_bid)
{
    CAuctionBid msg;
    msg.timestamp = T;
    msg.auctionId = Fill256(0xa1);
    msg.bidId = Fill256(0xa2);
    msg.bidder = FillKey(0xc1);
    msg.bidAmount = 6000 * COIN;
    msg.depositAmount = 900 * COIN;
    const std::string hex =
        "00f1536500000000a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a2a2a2a2a2a2a2a2"
        "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c10070c9b2"
        "8b00000000046bf414000000";
    CAuctionBid decoded = CheckVector(msg, hex);
    BOOST_CHECK(decoded.auctionId == msg.auctionId);
    BOOST_CHECK(decoded.bidId == msg.bidId);
    BOOST_CHECK(decoded.bidder == msg.bidder);
    BOOST_CHECK_EQUAL(decoded.bidAmount, 6000 * COIN);
    BOOST_CHECK_EQUAL(decoded.depositAmount, 900 * COIN);
    CheckTruncated<CAuctionBid>(hex);
}

BOOST_AUTO_TEST_CASE(auction_finalize)
{
    CAuctionFinalize msg;
    msg.timestamp = T;
    msg.auctionId = Fill256(0xa1);
    msg.winningBidId = Fill256(0xa2);
    msg.finalPrice = 6000 * COIN;
    msg.creator = FillKey(0xc1);
    const std::string hex =
        "00f1536500000000a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a2a2a2a2a2a2a2a2"
        "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a20070c9b28b000000c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1"
        "c1c1c1c1";
    CAuctionFinalize decoded = CheckVector(msg, hex);
    BOOST_CHECK(decoded.auctionId == msg.auctionId);
    BOOST_CHECK(decoded.winningBidId == msg.winningBidId);
    BOOST_CHECK_EQUAL(decoded.finalPrice, 6000 * COIN);
    BOOST_CHECK(decoded.creator == msg.creator);
    CheckTruncated<CAuctionFinalize>(hex);
}

BOOST_AUTO_TEST_CASE(userid_auction_announce)
{
    CUserIDAuctionAnnounce msg;
    msg.timestamp = T;
    msg.auctionId = Fill256(0xa1);
    msg.name = "alice";
    msg.namespaceId = "redd";
    msg.creator = FillKey(0xc1);
    msg.startTime = T + 1;
    msg.endTime = T + 2;
    msg.reservePrice = 5000 * COIN;
    msg.type = AUCTION_VERIFIED;
    const std::string hex =
        "00f1536500000000a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a105616c6963650472"
        "656464c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c101f153650000000002f15365000000000088526a7400000002";
    CUserIDAuctionAnnounce decoded = CheckVector(msg, hex);
    BOOST_CHECK(decoded.auctionId == msg.auctionId);
    BOOST_CHECK_EQUAL(decoded.name, "alice");
    BOOST_CHECK_EQUAL(decoded.namespaceId, "redd");
    BOOST_CHECK(decoded.creator == msg.creator);
    BOOST_CHECK_EQUAL(decoded.startTime, T + 1);
    BOOST_CHECK_EQUAL(decoded.endTime, T + 2);
    BOOST_CHECK_EQUAL(decoded.reservePrice, 5000 * COIN);
    BOOST_CHECK(decoded.type == AUCTION_VERIFIED);
    CheckTruncated<CUserIDAuctionAnnounce>(hex);
}

BOOST_AUTO_TEST_CASE(profile_update)
{
    CReddIDProfileUpdate msg;
    msg.timestamp = T;
    msg.reddId = "alice";
    msg.profileHash = Fill256(0xa3);
    msg.profile.reddId = "alice";
    msg.profile.owner = FillKey(0xc1);
    msg.profile.displayName = "Alice";
    msg.profile.avatarHash = Fill256(0xa1);
    msg.profile.bio = "bio";
    msg.profile.emailHash = Fill256(0xa2);
    msg.profile.socialData = "{}";
    msg.profile.messagingPubkey = {0x02, 0x03};
    msg.profile.verificationStatus = VERIFICATION_COMMUNITY;
    msg.profile.creationTime = T + 1;
    msg.profile.lastUpdated = T + 2;
    msg.profile.expirationTime = T + 3;
    msg.profile.lastActivityTime = T + 4;
    msg.profile.active = true;
    msg.profile.flags = 5;
    const std::string hex =
        "00f153650000000005616c696365a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a30561"
        "6c696365c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c105416c696365a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
        "a1a1a1a1a1a1a1a1a1a1a1a1a1a10362696fa2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2"
        "a2a2027b7d0202030201f153650000000002f153650000000003f153650000000004f15365000000000105";
    CReddIDProfileUpdate decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.reddId, "alice");
    BOOST_CHECK(decoded.profileHash == msg.profileHash);
    BOOST_CHECK_EQUAL(decoded.profile.reddId, "alice");
    BOOST_CHECK(decoded.profile.owner == msg.profile.owner);
    BOOST_CHECK_EQUAL(decoded.profile.displayName, "Alice");
    BOOST_CHECK(decoded.profile.avatarHash == msg.profile.avatarHash);
    BOOST_CHECK_EQUAL(decoded.profile.bio, "bio");
    BOOST_CHECK(decoded.profile.emailHash == msg.profile.emailHash);
    BOOST_CHECK_EQUAL(decoded.profile.socialData, "{}");
    BOOST_CHECK(decoded.profile.messagingPubkey == msg.profile.messagingPubkey);
    BOOST_CHECK(decoded.profile.verificationStatus == VERIFICATION_COMMUNITY);
    BOOST_CHECK_EQUAL(decoded.profile.creationTime, T + 1);
    BOOST_CHECK_EQUAL(decoded.profile.lastUpdated, T + 2);
    BOOST_CHECK_EQUAL(decoded.profile.expirationTime, T + 3);
    BOOST_CHECK_EQUAL(decoded.profile.lastActivityTime, T + 4);
    BOOST_CHECK(decoded.profile.active);
    BOOST_CHECK_EQUAL(decoded.profile.flags, 5);
    CheckTruncated<CReddIDProfileUpdate>(hex);
}

BOOST_AUTO_TEST_CASE(profile_request)
{
    CReddIDProfileRequest msg;
    msg.timestamp = T;
    msg.reddId = "alice";
    const std::string hex =
        "00f153650000000005616c696365";
    CReddIDProfileRequest decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.reddId, "alice");
    CheckTruncated<CReddIDProfileRequest>(hex);
}

BOOST_AUTO_TEST_CASE(connection)
{
    CReddIDConnection msg;
    msg.timestamp = T;
    msg.connection.fromReddId = "alice";
    msg.connection.toReddId = "bob";
    msg.connection.connectionType = CONNECTION_BLOCK;
    msg.connection.creationTime = T + 1;
    msg.connection.lastInteraction = T + 2;
    msg.connection.visibility = 1;
    msg.connection.metadata = "meta";
    msg.connection.txid = Fill256(0xa1);
    const std::string hex =
        "00f153650000000005616c69636503626f620301f153650000000002f153650000000001000000046d657461a1a1a1a1"
        "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1";
    CReddIDConnection decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.connection.fromReddId, "alice");
    BOOST_CHECK_EQUAL(decoded.connection.toReddId, "bob");
    BOOST_CHECK(decoded.connection.connectionType == CONNECTION_BLOCK);
    BOOST_CHECK_EQUAL(decoded.connection.creationTime, T + 1);
    BOOST_CHECK_EQUAL(decoded.connection.lastInteraction, T + 2);
    BOOST_CHECK_EQUAL(decoded.connection.visibility, 1);
    BOOST_CHECK_EQUAL(decoded.connection.metadata, "meta");
    BOOST_CHECK(decoded.connection.txid == msg.connection.txid);
    CheckTruncated<CReddIDConnection>(hex);
}

BOOST_AUTO_TEST_CASE(reputation_update)
{
    CReddIDReputationUpdate msg;
    msg.timestamp = T;
    msg.reputation.reddId = "alice";
    msg.reputation.overallScore = 50.25;
    msg.reputation.longevityScore = 1;
    msg.reputation.reliabilityScore = 2;
    msg.reputation.engagementScore = 3;
    msg.reputation.verificationScore = 4;
    msg.reputation.auctionScore = 100;
    msg.reputation.lastCalculated = T + 1;
    msg.reputation.calculationProof = Fill256(0xa1);
    msg.reputation.calculatorSignatures = "[]";
    const std::string hex =
        "00f153650000000005616c696365a113000064000000c80000002c010000900100001027000001f1536500000000a1a1"
        "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1025b5d";
    CReddIDReputationUpdate decoded = CheckVector(msg, hex);
    BOOST_CHECK_EQUAL(decoded.reputation.reddId, "alice");
    // The scores travel as hundredths in 32 bit integers
    BOOST_CHECK_EQUAL(decoded.reputation.overallScore, 50.25);
    BOOST_CHECK_EQUAL(decoded.reputation.longevityScore, 1);
    BOOST_CHECK_EQUAL(decoded.reputation.reliabilityScore, 2);
    BOOST_CHECK_EQUAL(decoded.reputation.engagementScore, 3);
    BOOST_CHECK_EQUAL(decoded.reputation.verificationScore, 4);
    BOOST_CHECK_EQUAL(decoded.reputation.auctionScore, 100);
    BOOST_CHECK_EQUAL(decoded.reputation.lastCalculated, T + 1);
    BOOST_CHECK(decoded.reputation.calculationProof == msg.reputation.calculationProof);
    BOOST_CHECK_EQUAL(decoded.reputation.calculatorSignatures, "[]");
    CheckTruncated<CReddIDReputationUpdate>(hex);
}

// The two cancel messages carry a bare auction ID and the namespace
// configuration request a bare string: no timestamp, no message class.
BOOST_AUTO_TEST_CASE(messages_without_a_class)
{
    BOOST_CHECK_EQUAL(Hex(Fill256(0xa1)), "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1");
    BOOST_CHECK_EQUAL(Hex(std::string("redd")), "0472656464");
}

// Current behaviour: the auction type is one byte that is cast to the enum
// without a check, so a message can carry a type that no enumerator names.
BOOST_AUTO_TEST_CASE(auction_type_byte_is_not_checked)
{
    CUserIDAuctionAnnounce msg;
    msg.startTime = 0;
    msg.endTime = 0;
    msg.reservePrice = 0;
    msg.type = AUCTION_STANDARD;
    std::string hex = Hex(msg);
    BOOST_CHECK_EQUAL(hex.substr(hex.size() - 2), "00");
    hex.replace(hex.size() - 2, 2, "ff");
    CUserIDAuctionAnnounce decoded = FromHex<CUserIDAuctionAnnounce>(hex);
    BOOST_CHECK_EQUAL(static_cast<int>(decoded.type), 255);
    BOOST_CHECK_EQUAL(Hex(decoded), hex);
}

// Strings and vectors are length-prefixed and limited only by what the stream
// holds: the message classes put no bound on socialData, messagingPubkey,
// metadata or calculatorSignatures.
BOOST_AUTO_TEST_CASE(variable_length_fields_have_no_bound_of_their_own)
{
    CReddIDProfileUpdate profile;
    profile.profile.socialData = std::string(100000, 's');
    profile.profile.messagingPubkey = std::vector<unsigned char>(100000, 0x02);
    CReddIDProfileUpdate decodedProfile = FromHex<CReddIDProfileUpdate>(Hex(profile));
    BOOST_CHECK_EQUAL(decodedProfile.profile.socialData.size(), 100000U);
    BOOST_CHECK_EQUAL(decodedProfile.profile.messagingPubkey.size(), 100000U);

    CReddIDConnection connection;
    connection.connection.metadata = std::string(100000, 'm');
    BOOST_CHECK_EQUAL(FromHex<CReddIDConnection>(Hex(connection)).connection.metadata.size(), 100000U);

    CReddIDReputationUpdate reputation;
    reputation.reputation.calculatorSignatures = std::string(100000, 'c');
    BOOST_CHECK_EQUAL(FromHex<CReddIDReputationUpdate>(Hex(reputation)).reputation.calculatorSignatures.size(), 100000U);
}

BOOST_AUTO_TEST_SUITE_END()
