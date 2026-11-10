// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_UPGRADEINDICATORTESTS_H
#define BITCOIN_QT_TEST_UPGRADEINDICATORTESTS_H

#include <QObject>
#include <QTest>

/**
 * The status bar indicator for network upgrades and fork warnings.
 *
 * The indicator shows what UpgradeIndicator::Evaluate() makes of the node's
 * SignallingInfo and warning kinds, so these hand it made-up values of both
 * and check the level, the reasons and the icon.
 */
class UpgradeIndicatorTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void hiddenWhenNothingToReport();
    void upgradeWhileAKnownDeploymentIsSignalledOrLockedIn();
    void amberForUnknownSignallingBlockVersionsAndInvalidChains();
    void redOnceAnUnknownRuleIsLockedInOrActive();
    void upgradeIconFollowsTheThemeAndWarningsDoNot();
};

#endif // BITCOIN_QT_TEST_UPGRADEINDICATORTESTS_H
