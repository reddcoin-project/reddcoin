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
    SetfLargeWorkInvalidChainFound(false);
    BOOST_CHECK(GetWarningKinds().empty());

    SetUpgradeWarnings({{WarningKind::UNKNOWN_RULE_LOCKED_IN, UnknownRuleLockedInWarning(27, 576, 1700000000)},
                        {WarningKind::UNKNOWN_BLOCK_VERSION, UnknownBlockVersionWarning(60, 100)}});
    BOOST_CHECK((GetWarningKinds() == std::set<WarningKind>{WarningKind::UNKNOWN_RULE_LOCKED_IN, WarningKind::UNKNOWN_BLOCK_VERSION}));
    // The texts are still the ones GetWarnings() shows.
    const std::string warnings = GetWarnings(true).original;
    BOOST_CHECK(warnings.find("versionbit 27") != std::string::npos);
    BOOST_CHECK(warnings.find("60 of the last 100 blocks") != std::string::npos);

    // The invalid chain warning is set elsewhere and reported with them.
    SetfLargeWorkInvalidChainFound(true);
    BOOST_CHECK(GetWarningKinds().count(WarningKind::INVALID_CHAIN));

    // Each kind goes away with its warning.
    SetUpgradeWarnings({});
    SetfLargeWorkInvalidChainFound(false);
    BOOST_CHECK(GetWarningKinds().empty());
    BOOST_CHECK(GetWarnings(true).original.find("versionbit") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
