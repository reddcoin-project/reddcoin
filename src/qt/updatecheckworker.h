// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_UPDATECHECKWORKER_H
#define BITCOIN_QT_UPDATECHECKWORKER_H

#include <QObject>
#include <QVariantMap>

class UniValue;

/**
 * Convert an update check result into the map the GUI reads.
 *
 * Copies every field rather than naming the ones wanted. An allow-list here
 * drifts the moment the node reports something new: the reader goes on asking
 * for a key nobody puts in, gets an empty string, and takes whichever branch
 * empty means. That is not hypothetical, it is what happened when phase 1 added
 * the artifact fields.
 *
 * Free and exposed so the mapping can be tested without constructing a node.
 */
QVariantMap UpdateInfoToVariantMap(const UniValue& result);

/**
 * Asks the node whether a newer release is available.
 *
 * The node performs a network request that can take seconds, so this must not
 * run on the GUI thread. Move an instance onto a QThread, connect something to
 * check() to start it and connect to checked() to receive the outcome. Both
 * connections cross a thread boundary, so Qt queues them automatically.
 */
class UpdateCheckWorker : public QObject
{
    Q_OBJECT

public:
    UpdateCheckWorker() = default;

public Q_SLOTS:
    //! Run the check on whichever thread this object lives on.
    void check();

Q_SIGNALS:
    //! Carries the fields of the checkupdates RPC, keyed by their RPC names.
    void checked(const QVariantMap& info);
};

#endif // BITCOIN_QT_UPDATECHECKWORKER_H
