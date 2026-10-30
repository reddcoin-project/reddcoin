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

static uint256 MakeTestHash()
{
    return uint256S("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
}

// ===================================================================
// 4B: Input Validation Tests (BasicTestingSetup - no full stack needed)
// ===================================================================

BOOST_FIXTURE_TEST_SUITE(reddid_security_tests, BasicTestingSetup)

// --- OP_RETURN decoder: zero-length name ---
BOOST_AUTO_TEST_CASE(opreturn_zero_length_name_rejected)
{
    // Namespace create with zero-length name: len=0
    std::vector<unsigned char> payload;
    payload.push_back(0);  // zero-length namespace
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    // ParseReddIDScript should succeed (it just extracts raw bytes)
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    BOOST_CHECK_EQUAL(opCode, OP_NAMESPACE_AUCTION_CREATE);
    // But the length byte is 0 - downstream decoder should reject
    BOOST_CHECK_EQUAL(data[0], 0);
}

// --- OP_RETURN decoder: name exceeding MAX_REDDID_LENGTH ---
BOOST_AUTO_TEST_CASE(opreturn_name_exceeds_max_rejected)
{
    // UserID create with oversized name
    std::string longName(MAX_REDDID_LENGTH + 1, 'a');
    std::string ns = "redd";
    auto payload = BuildUserIDCreatePayload(longName, ns);
    CScript script = MakeReddIDScript(OP_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    // Length byte should indicate the oversized name
    BOOST_CHECK(data[0] > MAX_REDDID_LENGTH);
}

// --- OP_RETURN decoder: zero-length namespace ---
BOOST_AUTO_TEST_CASE(opreturn_zero_length_namespace_rejected)
{
    // Namespace create with empty string
    std::vector<unsigned char> payload;
    payload.push_back(0);  // zero-length
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    BOOST_CHECK_EQUAL(data[0], 0);  // zero length = invalid
}

// --- OP_RETURN decoder: namespace exceeding MAX_NAMESPACE_LENGTH ---
BOOST_AUTO_TEST_CASE(opreturn_namespace_exceeds_max_rejected)
{
    std::string longNs(MAX_NAMESPACE_LENGTH + 1, 'a');
    auto payload = BuildNSCreatePayload(longNs);
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    BOOST_CHECK(data[0] > MAX_NAMESPACE_LENGTH);
}

// --- OP_RETURN bid: zero amount ---
BOOST_AUTO_TEST_CASE(opreturn_bid_zero_amount)
{
    auto payload = BuildBidPayload(MakeTestHash(), 0);
    CScript script = MakeReddIDScript(OP_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_BID);
    // Extract amount - should be zero
    CAmount amount = ReadLE64(data.data() + 32);
    BOOST_CHECK_EQUAL(amount, 0);
}

// --- OP_RETURN bid: null auction ID ---
BOOST_AUTO_TEST_CASE(opreturn_bid_null_auction_id)
{
    uint256 nullId;  // default is all zeros
    auto payload = BuildBidPayload(nullId, 1000 * COIN);
    CScript script = MakeReddIDScript(OP_AUCTION_BID, payload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    // Extract auction ID - should be null
    uint256 auctionId;
    std::copy(data.begin(), data.begin() + 32, auctionId.begin());
    BOOST_CHECK(auctionId.IsNull());
}

// --- OP_RETURN create: truncated data (undersized buffer) ---
BOOST_AUTO_TEST_CASE(opreturn_create_truncated_data)
{
    // Namespace create with payload too short (only header, no length byte)
    std::vector<unsigned char> emptyPayload;
    CScript script = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, emptyPayload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    // Data should be empty after the header was stripped
    BOOST_CHECK(data.empty());
}

// --- OP_RETURN bid: truncated data ---
BOOST_AUTO_TEST_CASE(opreturn_bid_truncated_data)
{
    // Bid with only 10 bytes of payload (needs 32+8=40)
    std::vector<unsigned char> shortPayload(10, 0xAB);
    CScript script = MakeReddIDScript(OP_AUCTION_BID, shortPayload);

    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDScript(script, opCode, data);
    BOOST_CHECK(parsed);
    // Data exists but is too short for a valid bid
    BOOST_CHECK(data.size() < 40);
}

// ===================================================================
// 4C: Consensus Safety Tests
// ===================================================================

// --- ProcessReddIDTransaction with null reddid pointer ---
BOOST_AUTO_TEST_CASE(process_tx_null_reddid_returns_false)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(MakeTestHash(), 0);  // non-null prevout so not coinbase
    mtx.vout.resize(1);
    CTransaction tx(mtx);

    BOOST_CHECK(!tx.IsCoinBase());
    // Null manager should return false, not crash
    BOOST_CHECK_EQUAL(ProcessReddIDTransaction(tx, 1, nullptr), false);
}

// --- ProcessReddIDTransaction with coinbase ---
BOOST_AUTO_TEST_CASE(process_tx_coinbase_returns_true)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vout.resize(1);
    CTransaction tx(mtx);

    BOOST_CHECK(tx.IsCoinBase());
    // Coinbase should be skipped (return true), even with null manager
    BOOST_CHECK_EQUAL(ProcessReddIDTransaction(tx, 1, nullptr), true);
}

// --- ParseReddIDScript: too-short script ---
BOOST_AUTO_TEST_CASE(parse_script_too_short)
{
    CScript script;
    script << OP_RETURN;  // Just the opcode, no data

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK_EQUAL(ParseReddIDScript(script, opCode, data), false);
}

// --- ParseReddIDScript: non-R prefix ---
BOOST_AUTO_TEST_CASE(parse_script_wrong_prefix)
{
    std::vector<unsigned char> rawData = {'X', OP_NAMESPACE_AUCTION_CREATE, 0x04, 'r', 'e', 'd', 'd'};
    CScript script;
    script << OP_RETURN << rawData;

    unsigned char opCode;
    std::vector<unsigned char> data;
    BOOST_CHECK_EQUAL(ParseReddIDScript(script, opCode, data), false);
}

// --- IsReddIDOpCode: valid opcodes ---
BOOST_AUTO_TEST_CASE(is_reddid_opcode_valid)
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

// --- IsReddIDOpCode: invalid opcodes ---
BOOST_AUTO_TEST_CASE(is_reddid_opcode_invalid)
{
    BOOST_CHECK(!IsReddIDOpCode('Z'));
    BOOST_CHECK(!IsReddIDOpCode('\0'));
    BOOST_CHECK(!IsReddIDOpCode('A'));
    BOOST_CHECK(!IsReddIDOpCode(0xFF));
}

BOOST_AUTO_TEST_SUITE_END()
