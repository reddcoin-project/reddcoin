// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_UPDATEDOWNLOADWORKER_H
#define BITCOIN_QT_UPDATEDOWNLOADWORKER_H

#include <QObject>
#include <QString>

#include <atomic>

/**
 * Downloads a release and verifies it, off the GUI thread.
 *
 * The node's staging call blocks for as long as a 29 MB transfer takes, so this
 * must not run on the GUI thread. Move an instance onto a QThread, connect
 * something to download() to start it, and connect to progressed() and
 * finished() to follow it. Both cross a thread boundary, so Qt queues them.
 *
 * A verified file is the only thing this produces. Nothing is installed and
 * nothing is run.
 */
class UpdateDownloadWorker : public QObject
{
    Q_OBJECT

public:
    UpdateDownloadWorker(QString version, QString artifact)
        : m_version{std::move(version)}, m_artifact{std::move(artifact)} {}

public Q_SLOTS:
    //! Run the download on whichever thread this object lives on.
    void download();

    /**
     * Ask the download to stop.
     *
     * Safe to call from the GUI thread while download() is running, which is
     * the point: it is the only member that is. The flag is read between reads
     * of the socket, so cancelling takes effect at the next one rather than
     * immediately, and never tears down an in-flight operation from underneath
     * the thread performing it.
     */
    void cancel() { m_cancelled = true; }

Q_SIGNALS:
    /**
     * Progress so far, and the total when the server declared one.
     *
     * Throttled to whole percentage points. The underlying callback fires
     * around 180 times a second on a 29 MB artifact, which is a reasonable rate
     * for a callback and far too high to drive a repaint, so the thinning
     * happens here rather than leaving every consumer to remember it.
     */
    void progressed(qint64 received, qint64 total);

    //! path and size are set when ok is true; error explains it when false, and
    //! is empty when the download was cancelled, since that is not a failure.
    void finished(bool ok, const QString& path, qint64 size, const QString& error);

private:
    QString m_version;
    QString m_artifact;
    std::atomic<bool> m_cancelled{false};
};

#endif // BITCOIN_QT_UPDATEDOWNLOADWORKER_H
