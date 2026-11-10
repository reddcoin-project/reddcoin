// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_NETWORKUPGRADESTESTS_H
#define BITCOIN_QT_TEST_NETWORKUPGRADESTESTS_H

#include <QObject>
#include <QTest>

/**
 * The Network Upgrades tab of the debug window.
 *
 * The tab only displays what interfaces::Node::getSignallingInfo() returns, so
 * these fill it with hand-made SignallingInfo values, the same structure the
 * node hands over, and check what each row shows.
 */
class NetworkUpgradesTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void headerNamesTheWindowOfTheNextBlock();
    void startedDeploymentShowsProgressAgainstTheThreshold();
    void lockedInActiveAndUnscheduledRows();
    void unknownBitsShownOnlyWhileSignalledThisWindow();
    void updateReplacesTheRows();
};

#endif // BITCOIN_QT_TEST_NETWORKUPGRADESTESTS_H
