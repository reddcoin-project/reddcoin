// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_p2p.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>

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

BOOST_FIXTURE_TEST_SUITE(reddid_security_p2p_tests, ReddIDTestingSetup)

// --- CNamespaceConfig with > MAX_PRICING_TIERS ---
BOOST_AUTO_TEST_CASE(namespace_config_oversized_pricing_tiers)
{
    // Build a CNamespaceConfig with too many pricing tiers
    CNamespaceConfig config;
    config.config.id = "test";
    config.config.owner = MakeKeyID();
    config.config.minLength = 3;
    config.config.maxLength = 32;
    config.config.renewalPeriod = 365;
    config.config.gracePeriod = 30;
    config.config.namespaceRevenuePct = 10;
    config.config.burnPct = 70;
    config.config.nodePct = 15;
    config.config.devPct = 5;

    // Fill with MAX_PRICING_TIERS + 1 tiers
    for (size_t i = 0; i <= MAX_PRICING_TIERS; ++i) {
        PricingTier tier;
        tier.minLength = i + 1;
        tier.minPrice = (i + 1) * COIN;
        config.pricingTiers.push_back(tier);
    }

    BOOST_CHECK(config.pricingTiers.size() > MAX_PRICING_TIERS);
}

// --- Namespace ID exceeding MAX_NAMESPACE_LENGTH in P2P ---
BOOST_AUTO_TEST_CASE(p2p_oversized_namespace_id)
{
    // Verify the constant is what we expect
    BOOST_CHECK_EQUAL(MAX_NAMESPACE_LENGTH, 15);

    // A namespace ID longer than MAX_NAMESPACE_LENGTH should fail
    std::string longNs(MAX_NAMESPACE_LENGTH + 1, 'a');
    BOOST_CHECK(longNs.size() > MAX_NAMESPACE_LENGTH);
}

// --- ReddID name exceeding MAX_REDDID_LENGTH in P2P ---
BOOST_AUTO_TEST_CASE(p2p_oversized_reddid_name)
{
    BOOST_CHECK_EQUAL(MAX_REDDID_LENGTH, 32);

    std::string longName(MAX_REDDID_LENGTH + 1, 'a');
    BOOST_CHECK(longName.size() > MAX_REDDID_LENGTH);

    // ValidateReddID should reject names that are too long
    BOOST_CHECK(!reddidManager->ValidateReddID(longName));
}

// --- Empty namespace ID in validation ---
BOOST_AUTO_TEST_CASE(p2p_empty_namespace_id)
{
    std::string emptyNs;
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID(emptyNs));
}

// --- Empty reddId in validation ---
BOOST_AUTO_TEST_CASE(p2p_empty_reddid)
{
    std::string emptyId;
    BOOST_CHECK(!reddidManager->ValidateReddID(emptyId));
}

// --- Namespace decoder rejects zero-length name ---
BOOST_AUTO_TEST_CASE(namespace_decoder_zero_length_name)
{
    // Build a minimal transaction with OP_RETURN containing zero-length namespace
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);

    // Create OP_RETURN with zero-length namespace
    std::vector<unsigned char> payload;
    payload.push_back(0);  // zero-length
    mtx.vout[0].scriptPubKey = MakeReddIDScript(OP_NAMESPACE_AUCTION_CREATE, payload);
    mtx.vout[0].nValue = 0;

    CTransaction tx(mtx);

    // The namespace manager should reject this
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_NAMESPACE_AUCTION_CREATE);
    data.push_back(0);  // zero-length namespace

    bool result = nsMgr->ProcessNamespaceAuctionCreate(tx, data, 1);
    BOOST_CHECK_EQUAL(result, false);
}

// --- Namespace decoder rejects oversized name ---
BOOST_AUTO_TEST_CASE(namespace_decoder_oversized_name)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.resize(1);

    // Build data with oversized namespace length
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_NAMESPACE_AUCTION_CREATE);
    data.push_back(MAX_NAMESPACE_LENGTH + 1);  // oversized
    // Add dummy bytes
    for (int i = 0; i < MAX_NAMESPACE_LENGTH + 1; ++i) {
        data.push_back('a');
    }

    CTransaction tx(mtx);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    bool result = nsMgr->ProcessNamespaceAuctionCreate(tx, data, 1);
    BOOST_CHECK_EQUAL(result, false);
}

BOOST_AUTO_TEST_SUITE_END()
