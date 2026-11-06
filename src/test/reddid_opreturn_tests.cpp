// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/transaction_handler.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <crypto/common.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

// Helper: Build an OP_RETURN script with ReddID header + payload
static CScript MakeReddIDScript(unsigned char opCode, const std::vector<unsigned char>& payload)
{
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(opCode);
    data.insert(data.end(), payload.begin(), payload.end());
    CScript script;
    script << OP_RETURN << data;
    return script;
}

// Helper: Build namespace create payload: len(1) + name
static std::vector<unsigned char> BuildNSCreatePayload(const std::string& ns)
{
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(ns.size()));
    payload.insert(payload.end(), ns.begin(), ns.end());
    return payload;
}

// Helper: Build bid payload: auctionId(32) + amount(8)
static std::vector<unsigned char> BuildBidPayload(const uint256& auctionId, CAmount amount)
{
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), auctionId.begin(), auctionId.end());
    std::vector<unsigned char> amountBytes(8);
    WriteLE64(amountBytes.data(), amount);
    payload.insert(payload.end(), amountBytes.begin(), amountBytes.end());
    return payload;
}

// Helper: Build userID create payload: len(1) + name + len(1) + namespace
static std::vector<unsigned char> BuildUserIDCreatePayload(const std::string& name, const std::string& ns)
{
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(name.size()));
    payload.insert(payload.end(), name.begin(), name.end());
    payload.push_back(static_cast<unsigned char>(ns.size()));
    payload.insert(payload.end(), ns.begin(), ns.end());
    return payload;
}

// Helper: Build profile update payload: len(1) + reddId + hash(32)
static std::vector<unsigned char> BuildProfileUpdatePayload(const std::string& reddId, const uint256& hash)
{
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(reddId.size()));
    payload.insert(payload.end(), reddId.begin(), reddId.end());
    payload.insert(payload.end(), hash.begin(), hash.end());
    return payload;
}

// Helper: Build connection payload: len(1) + from + len(1) + to + type(1)
static std::vector<unsigned char> BuildConnectionPayload(const std::string& from, const std::string& to, uint8_t type)
{
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(from.size()));
    payload.insert(payload.end(), from.begin(), from.end());
    payload.push_back(static_cast<unsigned char>(to.size()));
    payload.insert(payload.end(), to.begin(), to.end());
    payload.push_back(type);
    return payload;
}

static uint256 MakeTestHash()
{
    return uint256S("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
}

BOOST_FIXTURE_TEST_SUITE(reddid_opreturn_tests, BasicTestingSetup)

// ===================================================================
// Category 1: ParseReddIDScript validation
// ===================================================================

BOOST_AUTO_TEST_CASE(parse_valid_namespace_create)
{
    auto payload = BuildNSCreatePayload("redd");
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    // data should contain: len(1) + "redd"(4) = 5 bytes
    BOOST_CHECK_EQUAL(data.size(), 5u);
    BOOST_CHECK_EQUAL(data[0], 4); // length prefix
    BOOST_CHECK_EQUAL(data[1], 'r');
    BOOST_CHECK_EQUAL(data[4], 'd');
}

BOOST_AUTO_TEST_CASE(parse_valid_bid_script)
{
    uint256 auctionId = MakeTestHash();
    auto payload = BuildBidPayload(auctionId, 50000 * COIN);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_BID);
    // data: auctionId(32) + amount(8) = 40 bytes
    BOOST_CHECK_EQUAL(data.size(), 40u);
}

BOOST_AUTO_TEST_CASE(parse_empty_script_fails)
{
    CScript script;
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(!ParseReddIDScript(script, opCode, data));
}

BOOST_AUTO_TEST_CASE(parse_non_opreturn_fails)
{
    // Build a P2PKH-like script
    CScript script;
    script << OP_DUP << OP_HASH160;
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(!ParseReddIDScript(script, opCode, data));
}

BOOST_AUTO_TEST_CASE(parse_opreturn_no_data_push_fails)
{
    // Just OP_RETURN with no data
    CScript script;
    script << OP_RETURN;
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(!ParseReddIDScript(script, opCode, data));
}

BOOST_AUTO_TEST_CASE(parse_wrong_prefix_fails)
{
    // Use 'X' instead of 'R'
    std::vector<unsigned char> rawData;
    rawData.push_back('X');
    rawData.push_back(OP_NAMESPACE_AUCTION_CREATE);
    rawData.push_back(4);
    rawData.push_back('r'); rawData.push_back('e'); rawData.push_back('d'); rawData.push_back('d');
    CScript script;
    script << OP_RETURN << rawData;
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(!ParseReddIDScript(script, opCode, data));
}

BOOST_AUTO_TEST_CASE(parse_single_byte_data_fails)
{
    // Data push with only 'R' (no opcode)
    std::vector<unsigned char> rawData;
    rawData.push_back('R');
    CScript script;
    script << OP_RETURN << rawData;
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(!ParseReddIDScript(script, opCode, data));
}

BOOST_AUTO_TEST_CASE(parse_all_13_valid_opcodes)
{
    unsigned char validOpcodes[] = {
        OP_NAMESPACE_AUCTION_CREATE, OP_NAMESPACE_AUCTION_BID,
        OP_NAMESPACE_AUCTION_FINALIZE, OP_NAMESPACE_AUCTION_CANCEL,
        OP_AUCTION_CREATE, OP_AUCTION_BID,
        OP_AUCTION_FINALIZE, OP_AUCTION_CANCEL,
        OP_REDDID_AUCTION_CREATE, OP_REDDID_AUCTION_BID,
        OP_REDDID_AUCTION_FINALIZE, OP_REDDID_PROFILE_UPDATE,
        OP_REDDID_CONNECTION
    };

    for (unsigned char validOp : validOpcodes) {
        std::vector<unsigned char> emptyPayload;
        CScript script = MakeReddIDScript(validOp, emptyPayload);
        unsigned char opCode;
        std::vector<unsigned char> data;
        BOOST_CHECK(ParseReddIDScript(script, opCode, data));
        BOOST_CHECK_EQUAL(opCode, validOp);
    }
}

// ===================================================================
// Category 2: Encoding roundtrip tests
// ===================================================================

BOOST_AUTO_TEST_CASE(roundtrip_ns_create_redd)
{
    auto payload = BuildNSCreatePayload("redd");
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_CHECK_EQUAL(data[0], 4);
    std::string decoded(data.begin() + 1, data.begin() + 1 + data[0]);
    BOOST_CHECK_EQUAL(decoded, "redd");
}

BOOST_AUTO_TEST_CASE(roundtrip_ns_create_1char)
{
    auto payload = BuildNSCreatePayload("a");
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(data[0], 1);
    std::string decoded(data.begin() + 1, data.begin() + 2);
    BOOST_CHECK_EQUAL(decoded, "a");
}

BOOST_AUTO_TEST_CASE(roundtrip_ns_create_15char_max)
{
    std::string maxName(15, 'x');
    auto payload = BuildNSCreatePayload(maxName);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(data[0], 15);
    std::string decoded(data.begin() + 1, data.begin() + 16);
    BOOST_CHECK_EQUAL(decoded, maxName);
}

BOOST_AUTO_TEST_CASE(roundtrip_ns_bid)
{
    uint256 auctionId = MakeTestHash();
    CAmount amount = 50000 * COIN;
    auto payload = BuildBidPayload(auctionId, amount);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_BID);

    // Extract auctionId from data[0..31]
    uint256 parsedId;
    std::copy(data.begin(), data.begin() + 32, parsedId.begin());
    BOOST_CHECK(parsedId == auctionId);

    // Extract amount from data[32..39]
    CAmount parsedAmount = ReadLE64(data.data() + 32);
    BOOST_CHECK_EQUAL(parsedAmount, amount);
}

BOOST_AUTO_TEST_CASE(roundtrip_userid_create)
{
    auto payload = BuildUserIDCreatePayload("alice", "redd");
    CScript script = MakeReddIDScript(OP_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_CREATE);

    // Parse: len(1) + "alice"(5) + len(1) + "redd"(4) = 11 bytes
    BOOST_CHECK_EQUAL(data.size(), 11u);
    BOOST_CHECK_EQUAL(data[0], 5);
    std::string name(data.begin() + 1, data.begin() + 6);
    BOOST_CHECK_EQUAL(name, "alice");
    BOOST_CHECK_EQUAL(data[6], 4);
    std::string ns(data.begin() + 7, data.begin() + 11);
    BOOST_CHECK_EQUAL(ns, "redd");
}

BOOST_AUTO_TEST_CASE(roundtrip_userid_bid)
{
    uint256 auctionId = MakeTestHash();
    CAmount amount = 10000 * COIN;
    auto payload = BuildBidPayload(auctionId, amount);
    CScript script = MakeReddIDScript(OP_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_BID);
    BOOST_CHECK_EQUAL(data.size(), 40u);
}

BOOST_AUTO_TEST_CASE(roundtrip_profile_update)
{
    uint256 profileHash = MakeTestHash();
    auto payload = BuildProfileUpdatePayload("alice", profileHash);
    CScript script = MakeReddIDScript(OP_REDDID_PROFILE_UPDATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_REDDID_PROFILE_UPDATE);
    // len(1) + "alice"(5) + hash(32) = 38 bytes
    BOOST_CHECK_EQUAL(data.size(), 38u);
    BOOST_CHECK_EQUAL(data[0], 5);
    std::string reddId(data.begin() + 1, data.begin() + 6);
    BOOST_CHECK_EQUAL(reddId, "alice");
}

BOOST_AUTO_TEST_CASE(roundtrip_connection_follow)
{
    auto payload = BuildConnectionPayload("alice", "bob", CONNECTION_FOLLOW);
    CScript script = MakeReddIDScript(OP_REDDID_CONNECTION, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_REDDID_CONNECTION);
    // len(1)+"alice"(5)+len(1)+"bob"(3)+type(1) = 11 bytes
    BOOST_CHECK_EQUAL(data.size(), 11u);
    BOOST_CHECK_EQUAL(data[0], 5);
    BOOST_CHECK_EQUAL(data[6], 3);
    BOOST_CHECK_EQUAL(data[10], CONNECTION_FOLLOW);
}

BOOST_AUTO_TEST_CASE(roundtrip_connection_block)
{
    auto payload = BuildConnectionPayload("alice", "bob", CONNECTION_BLOCK);
    CScript script = MakeReddIDScript(OP_REDDID_CONNECTION, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(data[10], CONNECTION_BLOCK);
}

BOOST_AUTO_TEST_CASE(roundtrip_all_connection_types)
{
    SocialConnectionType types[] = {CONNECTION_FOLLOW, CONNECTION_FRIEND, CONNECTION_ENDORSE, CONNECTION_BLOCK};
    for (auto connType : types) {
        auto payload = BuildConnectionPayload("alice", "bob", static_cast<uint8_t>(connType));
        CScript script = MakeReddIDScript(OP_REDDID_CONNECTION, payload);
        unsigned char opCode;
        std::vector<unsigned char> data;
        BOOST_CHECK(ParseReddIDScript(script, opCode, data));
        BOOST_CHECK_EQUAL(data[10], static_cast<uint8_t>(connType));
    }
}

// ===================================================================
// Category 3: Boundary/overflow tests
// ===================================================================

BOOST_AUTO_TEST_CASE(boundary_max_payload_80_bytes)
{
    // Header (2) + 78 bytes payload = 80 total in the data push
    std::vector<unsigned char> payload(78, 0xAA);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 78u);
}

BOOST_AUTO_TEST_CASE(boundary_empty_payload_after_header)
{
    std::vector<unsigned char> emptyPayload;
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, emptyPayload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_CHECK(data.empty());
}

BOOST_AUTO_TEST_CASE(boundary_exact_byte_layout)
{
    auto payload = BuildNSCreatePayload("redd");
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);

    // First byte should be OP_RETURN (0x6a)
    BOOST_CHECK_EQUAL(script[0], OP_RETURN);
    // The pushdata opcode encodes the data length
    // Data is: 'R' + '{' + 0x04 + "redd" = 7 bytes
    // For 7 bytes, pushdata is just the length byte (7)
}

BOOST_AUTO_TEST_CASE(boundary_zero_length_prefix_string)
{
    // Name with zero-length prefix (empty string encoded)
    std::vector<unsigned char> payload;
    payload.push_back(0); // length = 0
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 1u);
    BOOST_CHECK_EQUAL(data[0], 0);
}

BOOST_AUTO_TEST_CASE(boundary_large_amount_encoding)
{
    uint256 auctionId = MakeTestHash();
    CAmount largeAmount = std::numeric_limits<int64_t>::max();
    auto payload = BuildBidPayload(auctionId, largeAmount);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK(ParseReddIDScript(script, opCode, data));

    CAmount parsedAmount = ReadLE64(data.data() + 32);
    BOOST_CHECK_EQUAL(parsedAmount, largeAmount);
}

BOOST_AUTO_TEST_CASE(is_reddid_opcode_valid_opcodes)
{
    BOOST_CHECK(IsReddIDOpCode(OP_NAMESPACE_AUCTION_CREATE));
    BOOST_CHECK(IsReddIDOpCode(OP_NAMESPACE_AUCTION_BID));
    BOOST_CHECK(IsReddIDOpCode(OP_NAMESPACE_AUCTION_FINALIZE));
    BOOST_CHECK(IsReddIDOpCode(OP_NAMESPACE_AUCTION_CANCEL));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_CREATE));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_BID));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_FINALIZE));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_CANCEL));
    BOOST_CHECK(IsReddIDOpCode(OP_REDDID_AUCTION_CREATE));
    BOOST_CHECK(IsReddIDOpCode(OP_REDDID_AUCTION_BID));
    BOOST_CHECK(IsReddIDOpCode(OP_REDDID_AUCTION_FINALIZE));
    BOOST_CHECK(IsReddIDOpCode(OP_REDDID_PROFILE_UPDATE));
    BOOST_CHECK(IsReddIDOpCode(OP_REDDID_CONNECTION));
}

BOOST_AUTO_TEST_CASE(is_reddid_opcode_invalid_opcodes)
{
    BOOST_CHECK(!IsReddIDOpCode('Z'));
    BOOST_CHECK(!IsReddIDOpCode('\0'));
    BOOST_CHECK(!IsReddIDOpCode('A'));
    BOOST_CHECK(!IsReddIDOpCode('a'));
    BOOST_CHECK(!IsReddIDOpCode(0xFF));
}

// ===================================================================
// Category 4: OP_RETURN builders and ParseReddIDOpReturn
// ===================================================================

// Parse a built script with both parsers and check that they agree.
static bool ParseBuilt(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data)
{
    unsigned char opCodeScript = 0;
    std::vector<unsigned char> dataScript;
    const bool parsed = ParseReddIDOpReturn(script, opCode, data);
    const bool parsedScript = ParseReddIDScript(script, opCodeScript, dataScript);
    BOOST_CHECK_EQUAL(parsed, parsedScript);
    if (parsed && parsedScript) {
        BOOST_CHECK_EQUAL(opCode, opCodeScript);
        BOOST_CHECK(data == dataScript);
    }
    return parsed;
}

static uint256 MakeOtherTestHash()
{
    return uint256S("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
}

BOOST_AUTO_TEST_CASE(build_namespace_auction_roundtrip)
{
    CScript script = CreateNamespaceAuctionOpReturn("redd");

    // OP_RETURN + direct push of 'R' + '{' + len + "redd" (7 bytes)
    BOOST_REQUIRE_EQUAL(script.size(), 9u);
    BOOST_CHECK_EQUAL(script[0], OP_RETURN);
    BOOST_CHECK_EQUAL(script[1], 7);
    BOOST_CHECK_EQUAL(script[2], 'R');
    BOOST_CHECK_EQUAL(script[3], OP_NAMESPACE_AUCTION_CREATE);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_CHECK(data == BuildNSCreatePayload("redd"));
    BOOST_CHECK(script == MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, BuildNSCreatePayload("redd")));
}

BOOST_AUTO_TEST_CASE(build_namespace_auction_size_limit)
{
    // Header (2) + length byte (1) + namespace: 77 characters fill the 80 byte payload.
    const std::string fits(77, 'n');
    CScript script = CreateNamespaceAuctionOpReturn(fits);
    // 80 bytes need OP_PUSHDATA1: OP_RETURN + OP_PUSHDATA1 + len + 80
    BOOST_REQUIRE_EQUAL(script.size(), 83u);
    BOOST_CHECK_EQUAL(script[1], OP_PUSHDATA1);
    BOOST_CHECK_EQUAL(script[2], REDDID_OP_RETURN_MAX_PAYLOAD);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_REQUIRE_EQUAL(data.size(), 78u);
    BOOST_CHECK_EQUAL(data[0], 77);
    BOOST_CHECK_EQUAL(std::string(data.begin() + 1, data.end()), fits);

    // One more character and the builder returns an empty script.
    BOOST_CHECK(CreateNamespaceAuctionOpReturn(std::string(78, 'n')).empty());
    // Far beyond what the one-byte length prefix could express.
    BOOST_CHECK(CreateNamespaceAuctionOpReturn(std::string(300, 'n')).empty());
}

BOOST_AUTO_TEST_CASE(build_namespace_auction_no_name_validation)
{
    // The builder only enforces the payload size. An empty namespace and one
    // longer than MAX_NAMESPACE_LENGTH are both encoded; rejecting them is
    // left to whoever decodes the payload.
    unsigned char opCode = 0;
    std::vector<unsigned char> data;

    CScript emptyNs = CreateNamespaceAuctionOpReturn("");
    BOOST_REQUIRE(ParseBuilt(emptyNs, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_REQUIRE_EQUAL(data.size(), 1u);
    BOOST_CHECK_EQUAL(data[0], 0);

    const std::string longNs(MAX_NAMESPACE_LENGTH + 1, 'a');
    CScript longScript = CreateNamespaceAuctionOpReturn(longNs);
    BOOST_REQUIRE(ParseBuilt(longScript, opCode, data));
    BOOST_REQUIRE_EQUAL(data.size(), longNs.size() + 1);
    BOOST_CHECK_EQUAL(data[0], MAX_NAMESPACE_LENGTH + 1);
}

BOOST_AUTO_TEST_CASE(build_userid_auction_roundtrip)
{
    CScript script = CreateUserIDAuctionOpReturn("alice", "redd");

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_CREATE);
    BOOST_CHECK(data == BuildUserIDCreatePayload("alice", "redd"));

    // Longest valid name in the longest valid namespace fits comfortably.
    const std::string maxName(MAX_REDDID_LENGTH, 'u');
    const std::string maxNs(MAX_NAMESPACE_LENGTH, 'n');
    CScript maxScript = CreateUserIDAuctionOpReturn(maxName, maxNs);
    BOOST_REQUIRE(ParseBuilt(maxScript, opCode, data));
    BOOST_REQUIRE_EQUAL(data.size(), 2u + MAX_REDDID_LENGTH + MAX_NAMESPACE_LENGTH);
    BOOST_CHECK_EQUAL(data[0], MAX_REDDID_LENGTH);
    BOOST_CHECK_EQUAL(std::string(data.begin() + 1, data.begin() + 1 + MAX_REDDID_LENGTH), maxName);
    BOOST_CHECK_EQUAL(data[1 + MAX_REDDID_LENGTH], MAX_NAMESPACE_LENGTH);
    BOOST_CHECK_EQUAL(std::string(data.begin() + 2 + MAX_REDDID_LENGTH, data.end()), maxNs);
}

BOOST_AUTO_TEST_CASE(build_userid_auction_size_limit)
{
    // Header (2) + two length bytes (2) + name + namespace <= 80
    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    CScript fits = CreateUserIDAuctionOpReturn(std::string(61, 'u'), std::string(15, 'n'));
    BOOST_REQUIRE(ParseBuilt(fits, opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 78u);

    BOOST_CHECK(CreateUserIDAuctionOpReturn(std::string(62, 'u'), std::string(15, 'n')).empty());
    BOOST_CHECK(CreateUserIDAuctionOpReturn(std::string(61, 'u'), std::string(16, 'n')).empty());
}

BOOST_AUTO_TEST_CASE(build_userid_bid_roundtrip)
{
    const uint256 auctionId = MakeTestHash();
    const CAmount amount = 12345 * COIN + 6789;
    CScript script = CreateUserIDBidOpReturn(auctionId, amount);

    // OP_RETURN + direct push of 42 bytes
    BOOST_REQUIRE_EQUAL(script.size(), 44u);
    BOOST_CHECK_EQUAL(script[1], 42);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_BID);
    BOOST_REQUIRE_EQUAL(data.size(), 40u);
    BOOST_CHECK(data == BuildBidPayload(auctionId, amount));

    uint256 parsedId;
    std::copy(data.begin(), data.begin() + 32, parsedId.begin());
    BOOST_CHECK(parsedId == auctionId);
    BOOST_CHECK_EQUAL(static_cast<CAmount>(ReadLE64(data.data() + 32)), amount);
}

BOOST_AUTO_TEST_CASE(build_userid_bid_no_amount_validation)
{
    // Zero, negative and maximal amounts are encoded as they are.
    const CAmount amounts[] = {0, -1, std::numeric_limits<CAmount>::min(), std::numeric_limits<CAmount>::max()};
    for (const CAmount amount : amounts) {
        CScript script = CreateUserIDBidOpReturn(uint256(), amount);
        unsigned char opCode = 0;
        std::vector<unsigned char> data;
        BOOST_REQUIRE(ParseBuilt(script, opCode, data));
        BOOST_REQUIRE_EQUAL(data.size(), 40u);
        BOOST_CHECK_EQUAL(static_cast<CAmount>(ReadLE64(data.data() + 32)), amount);
    }
}

BOOST_AUTO_TEST_CASE(build_profile_update_roundtrip)
{
    const uint256 profileHash = MakeTestHash();
    CScript script = CreateProfileUpdateOpReturn("alice", profileHash);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_REDDID_PROFILE_UPDATE);
    BOOST_CHECK(data == BuildProfileUpdatePayload("alice", profileHash));

    BOOST_REQUIRE_EQUAL(data.size(), 38u);
    uint256 parsedHash;
    std::copy(data.begin() + 6, data.begin() + 38, parsedHash.begin());
    BOOST_CHECK(parsedHash == profileHash);
}

BOOST_AUTO_TEST_CASE(build_profile_update_size_limit)
{
    // Header (2) + length byte (1) + reddId + hash (32) <= 80
    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    CScript fits = CreateProfileUpdateOpReturn(std::string(45, 'p'), MakeTestHash());
    BOOST_REQUIRE(ParseBuilt(fits, opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 78u);
    BOOST_CHECK_EQUAL(data[0], 45);

    BOOST_CHECK(CreateProfileUpdateOpReturn(std::string(46, 'p'), MakeTestHash()).empty());
}

BOOST_AUTO_TEST_CASE(build_connection_roundtrip)
{
    const SocialConnectionType types[] = {CONNECTION_FOLLOW, CONNECTION_FRIEND, CONNECTION_ENDORSE, CONNECTION_BLOCK};
    for (const SocialConnectionType type : types) {
        CScript script = CreateConnectionOpReturn("alice", "bob", type);
        unsigned char opCode = 0;
        std::vector<unsigned char> data;
        BOOST_REQUIRE(ParseBuilt(script, opCode, data));
        BOOST_CHECK_EQUAL(opCode, OP_REDDID_CONNECTION);
        BOOST_CHECK(data == BuildConnectionPayload("alice", "bob", static_cast<uint8_t>(type)));
    }

    // The type is written as a raw byte; values outside the enum are not rejected.
    CScript script = CreateConnectionOpReturn("alice", "bob", static_cast<SocialConnectionType>(200));
    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_REQUIRE_EQUAL(data.size(), 11u);
    BOOST_CHECK_EQUAL(data[10], 200);
}

BOOST_AUTO_TEST_CASE(build_connection_size_limit)
{
    // Header (2) + two length bytes (2) + from + to + type (1) <= 80
    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    CScript fits = CreateConnectionOpReturn(std::string(38, 'f'), std::string(37, 't'), CONNECTION_FRIEND);
    BOOST_REQUIRE(ParseBuilt(fits, opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 78u);
    BOOST_CHECK_EQUAL(data[77], CONNECTION_FRIEND);

    BOOST_CHECK(CreateConnectionOpReturn(std::string(38, 'f'), std::string(38, 't'), CONNECTION_FRIEND).empty());
}

BOOST_AUTO_TEST_CASE(build_bid_deposit_roundtrip)
{
    const uint256 auctionId = MakeTestHash();
    const CAmount amount = 7500 * COIN;
    const uint8_t depositVout = 3;
    const CKeyID bidder(uint160(ParseHex("0102030405060708090a0b0c0d0e0f1011121314")));

    CScript script = CreateBidDepositOpReturn(auctionId, amount, depositVout, bidder);

    // 'R' + 'D' + auctionId(32) + amount(8) + vout(1) + pubkey hash(20) = 63 bytes
    BOOST_REQUIRE_EQUAL(script.size(), 65u);
    BOOST_CHECK_EQUAL(script[1], 63);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_BID_DEPOSIT);
    BOOST_REQUIRE_EQUAL(data.size(), 61u);

    uint256 parsedId;
    std::copy(data.begin(), data.begin() + 32, parsedId.begin());
    BOOST_CHECK(parsedId == auctionId);
    BOOST_CHECK_EQUAL(static_cast<CAmount>(ReadLE64(data.data() + 32)), amount);
    BOOST_CHECK_EQUAL(data[40], depositVout);
    CKeyID parsedBidder;
    std::copy(data.begin() + 41, data.begin() + 61, parsedBidder.begin());
    BOOST_CHECK(parsedBidder == bidder);

    // The output index is a single byte; 255 is the largest that can be expressed.
    CScript maxVout = CreateBidDepositOpReturn(auctionId, amount, 255, bidder);
    BOOST_REQUIRE(ParseBuilt(maxVout, opCode, data));
    BOOST_CHECK_EQUAL(data[40], 255);
}

BOOST_AUTO_TEST_CASE(build_auction_settle_roundtrip)
{
    const uint256 auctionId = MakeTestHash();
    const uint256 bidId = MakeOtherTestHash();
    CScript script = CreateAuctionSettleOpReturn(auctionId, bidId);

    // OP_RETURN + direct push of 'R' + 'S' + auctionId(32) + bidId(32) = 66 bytes
    BOOST_REQUIRE_EQUAL(script.size(), 68u);
    BOOST_CHECK_EQUAL(script[1], 66);

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(ParseBuilt(script, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_SETTLE);
    BOOST_REQUIRE_EQUAL(data.size(), 64u);

    uint256 parsedAuction, parsedBid;
    std::copy(data.begin(), data.begin() + 32, parsedAuction.begin());
    std::copy(data.begin() + 32, data.begin() + 64, parsedBid.begin());
    BOOST_CHECK(parsedAuction == auctionId);
    BOOST_CHECK(parsedBid == bidId);
}

BOOST_AUTO_TEST_CASE(built_scripts_fit_payload_limit)
{
    // Every fixed-size builder stays within the payload limit, and every
    // script a builder returns starts with OP_RETURN and is unspendable.
    const CKeyID bidder(uint160(ParseHex("0102030405060708090a0b0c0d0e0f1011121314")));
    const CScript scripts[] = {
        CreateNamespaceAuctionOpReturn(std::string(MAX_NAMESPACE_LENGTH, 'n')),
        CreateUserIDAuctionOpReturn(std::string(MAX_REDDID_LENGTH, 'u'), std::string(MAX_NAMESPACE_LENGTH, 'n')),
        CreateUserIDBidOpReturn(MakeTestHash(), 1 * COIN),
        CreateProfileUpdateOpReturn(std::string(MAX_REDDID_LENGTH, 'u'), MakeTestHash()),
        CreateConnectionOpReturn(std::string(MAX_REDDID_LENGTH, 'a'), std::string(MAX_REDDID_LENGTH, 'b'), CONNECTION_FOLLOW),
        CreateBidDepositOpReturn(MakeTestHash(), 1 * COIN, 0, bidder),
        CreateAuctionSettleOpReturn(MakeTestHash(), MakeOtherTestHash()),
    };
    for (const CScript& script : scripts) {
        BOOST_REQUIRE(!script.empty());
        BOOST_CHECK(script.IsUnspendable());
        unsigned char opCode = 0;
        std::vector<unsigned char> data;
        BOOST_REQUIRE(ParseBuilt(script, opCode, data));
        BOOST_CHECK(IsReddIDOpCode(opCode));
        BOOST_CHECK(data.size() + 2 <= REDDID_OP_RETURN_MAX_PAYLOAD);
    }
}

BOOST_AUTO_TEST_CASE(opreturn_parser_rejects_malformed)
{
    // Sentinels show that a failed parse leaves the outputs untouched.
    unsigned char opCode = 0x42;
    std::vector<unsigned char> data{0x99};

    // Empty script (what a builder returns for an oversized payload)
    BOOST_CHECK(!ParseReddIDOpReturn(CScript(), opCode, data));

    // OP_RETURN alone
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN, opCode, data));

    // Not an OP_RETURN script
    std::vector<unsigned char> good{'R', OP_AUCTION_BID, 0x01};
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_DUP << good, opCode, data));
    // OP_RETURN not in first position
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_NOP << OP_RETURN << good, opCode, data));

    // OP_RETURN followed by an opcode that pushes no data
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN << OP_1, opCode, data));
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN << OP_0, opCode, data));

    // Push shorter than the two-byte header
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN << std::vector<unsigned char>{'R'}, opCode, data));

    // Wrong prefix, including lower case
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN << std::vector<unsigned char>{'r', OP_AUCTION_BID, 0x01}, opCode, data));
    BOOST_CHECK(!ParseReddIDOpReturn(CScript() << OP_RETURN << std::vector<unsigned char>{OP_AUCTION_BID, 'R', 0x01}, opCode, data));

    // Truncated push: the length byte promises 10 bytes, 3 follow
    CScript truncated;
    truncated << OP_RETURN;
    truncated.push_back(10);
    truncated.push_back('R');
    truncated.push_back(OP_AUCTION_BID);
    truncated.push_back(0x01);
    BOOST_CHECK(!ParseReddIDOpReturn(truncated, opCode, data));
    BOOST_CHECK(!ParseReddIDScript(truncated, opCode, data));

    // Truncated OP_PUSHDATA1: the length byte itself is missing
    CScript noLength;
    noLength << OP_RETURN;
    noLength.push_back(OP_PUSHDATA1);
    BOOST_CHECK(!ParseReddIDOpReturn(noLength, opCode, data));

    // A built script cut short anywhere inside its push no longer parses
    const CScript built = CreateUserIDBidOpReturn(MakeTestHash(), 5 * COIN);
    for (size_t len = 0; len < built.size(); ++len) {
        const CScript cut(built.begin(), built.begin() + len);
        BOOST_CHECK(!ParseReddIDOpReturn(cut, opCode, data));
    }

    BOOST_CHECK_EQUAL(opCode, 0x42);
    BOOST_REQUIRE_EQUAL(data.size(), 1u);
    BOOST_CHECK_EQUAL(data[0], 0x99);
}

BOOST_AUTO_TEST_CASE(opreturn_parser_is_permissive)
{
    // The parser only splits the push into opcode and data. It does not check
    // that the opcode is a ReddID opcode, that the payload is within the
    // builders' size limit, or that nothing follows the push.
    unsigned char opCode = 0;
    std::vector<unsigned char> data;

    // Unknown opcode
    BOOST_CHECK(!IsReddIDOpCode('Z'));
    BOOST_REQUIRE(ParseReddIDOpReturn(MakeReddIDScript('Z', {0x01, 0x02}), opCode, data));
    BOOST_CHECK_EQUAL(opCode, 'Z');
    BOOST_CHECK_EQUAL(data.size(), 2u);

    // Header only: data comes back empty
    data = {0x99};
    BOOST_REQUIRE(ParseReddIDOpReturn(MakeReddIDScript(OP_AUCTION_SETTLE, {}), opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_SETTLE);
    BOOST_CHECK(data.empty());

    // One byte over the builders' limit (OP_PUSHDATA1)
    BOOST_REQUIRE(ParseReddIDOpReturn(MakeReddIDScript(OP_AUCTION_BID, std::vector<unsigned char>(79, 0xAA)), opCode, data));
    BOOST_CHECK_EQUAL(data.size(), 79u);

    // Far over the limit (OP_PUSHDATA2), beyond MAX_SCRIPT_ELEMENT_SIZE too
    BOOST_REQUIRE(ParseReddIDOpReturn(MakeReddIDScript(OP_AUCTION_BID, std::vector<unsigned char>(1000, 0xBB)), opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_BID);
    BOOST_CHECK_EQUAL(data.size(), 1000u);

    // Trailing opcodes and pushes after the first push are ignored
    CScript trailing = CreateNamespaceAuctionOpReturn("redd");
    trailing << OP_DUP << std::vector<unsigned char>{'R', OP_AUCTION_BID, 0x07};
    BOOST_REQUIRE(ParseReddIDOpReturn(trailing, opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    BOOST_CHECK(data == BuildNSCreatePayload("redd"));

    // Both parsers behave the same on all of the above
    BOOST_CHECK(ParseBuilt(trailing, opCode, data));
    BOOST_CHECK(ParseBuilt(MakeReddIDScript('Z', {0x01}), opCode, data));
}

BOOST_AUTO_TEST_CASE(is_reddid_opcode_deposit_opcodes)
{
    BOOST_CHECK(IsReddIDOpCode(OP_BID_DEPOSIT));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_SETTLE));
    BOOST_CHECK(IsReddIDOpCode(OP_AUCTION_DEFAULT));
    BOOST_CHECK(IsReddIDOpCode(OP_DEPOSIT_FORFEIT));
}

BOOST_AUTO_TEST_CASE(is_reddid_opcode_exhaustive)
{
    // Exactly the 17 distinct operation codes are accepted, nothing else.
    int accepted = 0;
    for (int op = 0; op < 256; ++op) {
        if (IsReddIDOpCode(static_cast<unsigned char>(op))) ++accepted;
    }
    BOOST_CHECK_EQUAL(accepted, 17);
}

BOOST_AUTO_TEST_SUITE_END()
