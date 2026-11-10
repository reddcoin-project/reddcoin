// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/translation.h>
#include <warnings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <set>

BOOST_FIXTURE_TEST_SUITE(warnings_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(warning_kinds_follow_the_upgrade_warnings)
{
    SetUpgradeWarnings({});
    SetForkWarnings({});
    BOOST_CHECK(GetWarningKinds().empty());

    SetUpgradeWarnings({{WarningKind::UNKNOWN_RULE_LOCKED_IN, UnknownRuleLockedInWarning(27, 576, 1700000000)},
                        {WarningKind::UNKNOWN_BLOCK_VERSION, UnknownBlockVersionWarning(60, 100)}});
    BOOST_CHECK((GetWarningKinds() == std::set<WarningKind>{WarningKind::UNKNOWN_RULE_LOCKED_IN, WarningKind::UNKNOWN_BLOCK_VERSION}));
    // The texts are still the ones GetWarnings() shows.
    const std::string warnings = GetWarnings(true).original;
    BOOST_CHECK(warnings.find("versionbit 27") != std::string::npos);
    BOOST_CHECK(warnings.find("60 of the last 100 blocks") != std::string::npos);

    // The fork warnings are set separately and reported with them.
    SetForkWarnings({{WarningKind::INVALID_CHAIN, InvalidChainWarning(100, 120, 110)},
                     {WarningKind::HEAVIER_CHAIN, HeavierChainWarning(200, 230, 215, false)}});
    BOOST_CHECK((GetWarningKinds() == std::set<WarningKind>{WarningKind::UNKNOWN_RULE_LOCKED_IN, WarningKind::UNKNOWN_BLOCK_VERSION,
                                                            WarningKind::INVALID_CHAIN, WarningKind::HEAVIER_CHAIN}));

    // Each kind goes away with its warning.
    SetUpgradeWarnings({});
    BOOST_CHECK((GetWarningKinds() == std::set<WarningKind>{WarningKind::INVALID_CHAIN, WarningKind::HEAVIER_CHAIN}));
    BOOST_CHECK(GetWarnings(true).original.find("versionbit") == std::string::npos);
    SetForkWarnings({});
    BOOST_CHECK(GetWarningKinds().empty());
}

BOOST_AUTO_TEST_CASE(fork_warning_texts)
{
    SetUpgradeWarnings({});
    SetForkWarnings({});
    const std::string invalid = InvalidChainWarning(100, 120, 110).original;
    BOOST_CHECK(invalid.find("branches off at block 100 and reaches block 120") != std::string::npos);
    BOOST_CHECK(invalid.find("contains a block that this node considers invalid") != std::string::npos);
    BOOST_CHECK(invalid.find("its own chain at block 110") != std::string::npos);
    BOOST_CHECK(invalid.find(UPGRADE_URL) != std::string::npos);

    const std::string missing = HeavierChainWarning(200, 230, 215, false).original;
    BOOST_CHECK(missing.find("branches off at block 200 and reaches block 230") != std::string::npos);
    BOOST_CHECK(missing.find("have not arrived or could not be verified") != std::string::npos);
    BOOST_CHECK(missing.find("its own chain at block 215") != std::string::npos);
    BOOST_CHECK(HeavierChainWarning(200, 230, 215, true).original.find("have arrived and could not be connected") != std::string::npos);

    // The concise form shows a fork warning ahead of an upgrade warning: this
    // node may already be on the wrong chain.
    SetUpgradeWarnings({{WarningKind::UNKNOWN_BLOCK_VERSION, UnknownBlockVersionWarning(60, 100)}});
    SetForkWarnings({{WarningKind::HEAVIER_CHAIN, HeavierChainWarning(200, 230, 215, false)}});
    BOOST_CHECK_EQUAL(GetWarnings(false).original, missing);
    SetUpgradeWarnings({});
    SetForkWarnings({});
}

BOOST_AUTO_TEST_SUITE_END()
