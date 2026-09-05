// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatedownloadworker.h>

#include <node/release_verify.h>
#include <util/system.h>

#include <string>

void UpdateDownloadWorker::download()
{
    // Whole percentage points only. The callback below is invoked on every read
    // of the socket, roughly 180 times a second, and a queued signal per read
    // would spend more of the GUI thread on repainting than the transfer spends
    // on arriving.
    int last_percent{-1};

    const node::DownloadProgress progress = [&](int64_t received, int64_t total) {
        if (total <= 0) {
            // No Content-Length, so there is no percentage to report. Emit
            // sparingly on a byte boundary instead, which keeps a determinate
            // bar from being faked out of nothing.
            if (received / (1024 * 1024) == last_percent) return;
            last_percent = static_cast<int>(received / (1024 * 1024));
            Q_EMIT progressed(received, -1);
            return;
        }
        const int percent{static_cast<int>((received * 100) / total)};
        if (percent == last_percent) return;
        last_percent = percent;
        Q_EMIT progressed(received, total);
    };

    const node::DownloadCancel cancel = [this] { return m_cancelled.load(); };

    node::StagedRelease staged;
    std::string error;
    const bool ok = node::StageVerifiedRelease(
        m_version.toStdString(), m_artifact.toStdString(),
        gArgs.GetDataDirNet() / "updates", progress, cancel, staged, error);

    if (!ok) {
        // A cancelled download reports no error, because the user asking for it
        // to stop is not a failure to explain back to them.
        Q_EMIT finished(false, QString{}, 0, QString::fromStdString(error));
        return;
    }

    Q_EMIT finished(true, QString::fromStdString(staged.path.string()),
                    static_cast<qint64>(staged.size), QString{});
}
