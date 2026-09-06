// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_UPDATEDIALOGTESTS_H
#define BITCOIN_QT_TEST_UPDATEDIALOGTESTS_H

#include <QObject>
#include <QTest>

/**
 * The update dialog's download controls.
 *
 * None of these touch the network. What they cover is the wiring around the
 * download, which is where this can go wrong quietly: whether the controls
 * appear only when there is something to fetch, and whether a dialog carrying a
 * worker thread can be destroyed safely.
 */
class UpdateDialogTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyFieldTheDialogReadsSurvivesTheWorker();
    void downloadOfferedOnlyWithAnArtifact();
    void handOffWordingMatchesThePlatform();
    void destroyingTheDialogIsSafe();
};

#endif // BITCOIN_QT_TEST_UPDATEDIALOGTESTS_H
