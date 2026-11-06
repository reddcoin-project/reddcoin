// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/namespace.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <key.h>

#include <string>

BOOST_FIXTURE_TEST_SUITE(reddid_validation_tests, ReddIDTestingSetup)

// Helper: build a valid NamespaceInfo with all fields in valid ranges
static NamespaceInfo MakeValidNamespaceConfig(const std::string& id, const CKeyID& owner)
{
    NamespaceInfo ns;
    ns.id = id;
    ns.owner = owner;
    ns.minLength = 3;
    ns.maxLength = 32;
    ns.renewalPeriod = 365;
    ns.gracePeriod = 30;
    ns.namespaceRevenuePct = 10;
    ns.burnPct = 70;
    ns.nodePct = 15;
    ns.devPct = 5;
    ns.minAuctionDuration = 3;
    ns.maxAuctionDuration = 7;
    ns.minBidIncrement = 5.0;
    return ns;
}

// ===================================================================
// Category 1: ReddID name validation
// ===================================================================

BOOST_AUTO_TEST_CASE(reddid_valid_basic_name)
{
    BOOST_CHECK(reddidManager->ValidateReddID("alice"));
}

BOOST_AUTO_TEST_CASE(reddid_valid_minimum_length)
{
    BOOST_CHECK(reddidManager->ValidateReddID("abc"));
}

BOOST_AUTO_TEST_CASE(reddid_valid_maximum_length)
{
    std::string maxName(32, 'a');
    BOOST_CHECK(reddidManager->ValidateReddID(maxName));
}

BOOST_AUTO_TEST_CASE(reddid_valid_with_digits)
{
    BOOST_CHECK(reddidManager->ValidateReddID("alice123"));
}

BOOST_AUTO_TEST_CASE(reddid_valid_underscore_middle)
{
    BOOST_CHECK(reddidManager->ValidateReddID("a_b"));
}

BOOST_AUTO_TEST_CASE(reddid_valid_complex_name)
{
    BOOST_CHECK(reddidManager->ValidateReddID("test_name_123"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_too_short)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("ab"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_too_long)
{
    std::string longName(33, 'a');
    BOOST_CHECK(!reddidManager->ValidateReddID(longName));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_empty)
{
    BOOST_CHECK(!reddidManager->ValidateReddID(""));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_uppercase)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("Alice"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_special_char)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("alice@bob"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_space)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("alice bob"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_leading_underscore)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("_alice"));
}

BOOST_AUTO_TEST_CASE(reddid_invalid_trailing_underscore)
{
    BOOST_CHECK(!reddidManager->ValidateReddID("alice_"));
}

// ===================================================================
// Category 2: Namespace ID validation
// ===================================================================

BOOST_AUTO_TEST_CASE(namespace_valid_single_char)
{
    BOOST_CHECK(reddidManager->GetNamespaceManager()->ValidateNamespaceID("a"));
}

BOOST_AUTO_TEST_CASE(namespace_valid_typical)
{
    BOOST_CHECK(reddidManager->GetNamespaceManager()->ValidateNamespaceID("redd"));
}

BOOST_AUTO_TEST_CASE(namespace_valid_maximum_length)
{
    std::string maxNs(15, 'x');
    BOOST_CHECK(reddidManager->GetNamespaceManager()->ValidateNamespaceID(maxNs));
}

BOOST_AUTO_TEST_CASE(namespace_valid_with_digits)
{
    BOOST_CHECK(reddidManager->GetNamespaceManager()->ValidateNamespaceID("test123"));
}

BOOST_AUTO_TEST_CASE(namespace_valid_hyphen_middle)
{
    BOOST_CHECK(reddidManager->GetNamespaceManager()->ValidateNamespaceID("my-ns"));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_empty)
{
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID(""));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_too_long)
{
    std::string longNs(16, 'x');
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID(longNs));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_uppercase)
{
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID("REDD"));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_leading_hyphen)
{
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID("-redd"));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_trailing_hyphen)
{
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID("redd-"));
}

BOOST_AUTO_TEST_CASE(namespace_invalid_consecutive_hyphens)
{
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->ValidateNamespaceID("my--ns"));
}

// ===================================================================
// Category 3: Namespace config validation via CreateNamespace
// ===================================================================

BOOST_AUTO_TEST_CASE(nsconfig_valid_creates_ok)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid1", owner);
    BOOST_CHECK(reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_pct_sum_not_100)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid2", owner);
    ns.burnPct = 60; // 10 + 60 + 15 + 5 = 90
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_burn_pct_below_min)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid3", owner);
    ns.burnPct = 45;
    ns.nodePct = 40; // 10 + 45 + 40 + 5 = 100, but burnPct < 50
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_renewal_too_short)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid4", owner);
    ns.renewalPeriod = 30; // < 90
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_min_gt_max_length)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid5", owner);
    ns.minLength = 40;
    ns.maxLength = 10;
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_min_auction_gt_max_auction)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid6", owner);
    ns.minAuctionDuration = 10;
    ns.maxAuctionDuration = 5;
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_bid_increment_out_of_range)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid7", owner);
    ns.minBidIncrement = 25.0; // > 20.0
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(nsconfig_invalid_duplicate_namespace_id)
{
    CKeyID owner = MakeKeyID();
    NamespaceInfo ns = MakeValidNamespaceConfig("valid8", owner);
    BOOST_CHECK(reddidManager->GetNamespaceManager()->CreateNamespace(ns));
    NamespaceInfo ns2 = MakeValidNamespaceConfig("valid8", owner);
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns2));
}

// ===================================================================
// Category 4: User ID validation against the namespace configuration
// ===================================================================

BOOST_AUTO_TEST_CASE(userid_unknown_namespace_rejected)
{
    BOOST_CHECK(!reddidManager->ValidateUserID("alice", "nosuchns"));
}

BOOST_AUTO_TEST_CASE(userid_invalid_namespace_id_rejected)
{
    BOOST_CHECK(!reddidManager->ValidateUserID("alice", "BadNs"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice", ""));
}

BOOST_AUTO_TEST_CASE(userid_valid_names)
{
    // CreateTestNamespace allows digits, hyphens and underscores, length 3..32
    CreateTestNamespace("uidns1", MakeKeyID());
    BOOST_CHECK(reddidManager->ValidateUserID("alice", "uidns1"));
    BOOST_CHECK(reddidManager->ValidateUserID("abc", "uidns1"));
    BOOST_CHECK(reddidManager->ValidateUserID(std::string(32, 'a'), "uidns1"));
    BOOST_CHECK(reddidManager->ValidateUserID("alice123", "uidns1"));
    BOOST_CHECK(reddidManager->ValidateUserID("al-ice", "uidns1"));
    BOOST_CHECK(reddidManager->ValidateUserID("al_ice", "uidns1"));
}

BOOST_AUTO_TEST_CASE(userid_length_limits_come_from_namespace)
{
    CreateTestNamespace("uidns2", MakeKeyID());
    BOOST_CHECK(!reddidManager->ValidateUserID("", "uidns2"));
    BOOST_CHECK(!reddidManager->ValidateUserID("ab", "uidns2"));
    BOOST_CHECK(!reddidManager->ValidateUserID(std::string(33, 'a'), "uidns2"));

    NamespaceInfo ns = MakeValidNamespaceConfig("uidns2b", MakeKeyID());
    ns.minLength = 5;
    ns.maxLength = 6;
    BOOST_CHECK(reddidManager->GetNamespaceManager()->CreateNamespace(ns));
    BOOST_CHECK(!reddidManager->ValidateUserID("abcd", "uidns2b"));
    BOOST_CHECK(reddidManager->ValidateUserID("abcde", "uidns2b"));
    BOOST_CHECK(reddidManager->ValidateUserID("abcdef", "uidns2b"));
    BOOST_CHECK(!reddidManager->ValidateUserID("abcdefg", "uidns2b"));
}

BOOST_AUTO_TEST_CASE(userid_invalid_characters)
{
    CreateTestNamespace("uidns3", MakeKeyID());
    BOOST_CHECK(!reddidManager->ValidateUserID("Alice", "uidns3"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice bob", "uidns3"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice@bob", "uidns3"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice.bob", "uidns3"));
}

BOOST_AUTO_TEST_CASE(userid_leading_or_trailing_hyphen_rejected)
{
    CreateTestNamespace("uidns4", MakeKeyID());
    BOOST_CHECK(!reddidManager->ValidateUserID("-alice", "uidns4"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice-", "uidns4"));
}

// Current behaviour: unlike ValidateReddID, ValidateUserID accepts a leading or trailing
// underscore and consecutive hyphens; only the hyphen is checked at the ends.
BOOST_AUTO_TEST_CASE(userid_underscore_ends_and_double_hyphen_accepted)
{
    CreateTestNamespace("uidns5", MakeKeyID());
    BOOST_CHECK(reddidManager->ValidateUserID("_alice", "uidns5"));
    BOOST_CHECK(reddidManager->ValidateUserID("alice_", "uidns5"));
    BOOST_CHECK(reddidManager->ValidateUserID("al--ice", "uidns5"));
    BOOST_CHECK(!reddidManager->ValidateReddID("_alice"));
    BOOST_CHECK(!reddidManager->ValidateReddID("alice_"));
}

BOOST_AUTO_TEST_CASE(userid_character_classes_follow_namespace_flags)
{
    // A default-constructed NamespaceInfo allows letters only
    NamespaceInfo ns = MakeValidNamespaceConfig("uidns6", MakeKeyID());
    BOOST_CHECK(reddidManager->GetNamespaceManager()->CreateNamespace(ns));
    BOOST_CHECK(reddidManager->ValidateUserID("alice", "uidns6"));
    BOOST_CHECK(!reddidManager->ValidateUserID("alice1", "uidns6"));
    BOOST_CHECK(!reddidManager->ValidateUserID("al-ice", "uidns6"));
    BOOST_CHECK(!reddidManager->ValidateUserID("al_ice", "uidns6"));

    NamespaceInfo digits = MakeValidNamespaceConfig("uidns6d", MakeKeyID());
    digits.allowNumbers = true;
    BOOST_CHECK(reddidManager->GetNamespaceManager()->CreateNamespace(digits));
    BOOST_CHECK(reddidManager->ValidateUserID("alice1", "uidns6d"));
    BOOST_CHECK(!reddidManager->ValidateUserID("al-ice", "uidns6d"));
    BOOST_CHECK(!reddidManager->ValidateUserID("al_ice", "uidns6d"));
}

BOOST_AUTO_TEST_SUITE_END()
