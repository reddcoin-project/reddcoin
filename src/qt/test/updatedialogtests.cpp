// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/updatedialogtests.h>

#include <qt/networkstyle.h>
#include <qt/utilitydialog.h>

#include <QPushButton>
#include <QVariantMap>

namespace {
//! An update-check result, as showUpdateInfo receives it.
QVariantMap Result(const QString& local, const QString& remote, const QString& artifact)
{
    QVariantMap info;
    info["localversion"] = local;
    info["remoteversion"] = remote;
    info["updateavailable"] = local != remote;
    info["message"] = QString{};
    info["warning"] = QString{};
    info["errors"] = QString{};
    const QString base{QStringLiteral("https://download.reddcoin.com/bin/reddcoin-core-%1").arg(remote)};
    info["officialDownloadLink"] = base;
    info["platform"] = "x86_64-linux-gnu";
    info["guiartifact"] = artifact;
    info["guiartifactlink"] =
        artifact.isEmpty() ? QString{} : QStringLiteral("%1/%2").arg(base, artifact);
    return info;
}

//! Feed a result to the dialog the way the worker thread does.
void Deliver(HelpMessageDialog& dialog, const QVariantMap& info)
{
    QMetaObject::invokeMethod(&dialog, "showUpdateInfo", Qt::DirectConnection,
                              Q_ARG(QVariantMap, info));
}

int DownloadButtons(const HelpMessageDialog& dialog)
{
    int found{0};
    for (const QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text().contains("Download")) ++found;
    }
    return found;
}
} // namespace

void UpdateDialogTests::downloadOfferedOnlyWithAnArtifact()
{
    std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate("regtest")};
    QVERIFY(style);

    // Up to date: nothing to download, so nothing to offer.
    {
        HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
        Deliver(dialog, Result("4.22.9.4", "4.22.9.4", ""));
        QCOMPARE(DownloadButtons(dialog), 0);
    }

    // An update exists but no artifact is named for this host, which is the
    // prerelease and unpublished-host case. Offering a download would mean
    // offering a file whose name was never established.
    {
        HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
        Deliver(dialog, Result("4.22.9.0", "4.22.9.4", ""));
        QCOMPARE(DownloadButtons(dialog), 0);
    }

    // An update with a named artifact: this is the only case that gets a button.
    {
        HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
        Deliver(dialog, Result("4.22.9.0", "4.22.9.4", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz"));
        QCOMPARE(DownloadButtons(dialog), 1);
    }
}

void UpdateDialogTests::destroyingTheDialogIsSafe()
{
    // The worker lives on a thread the dialog owns and holds a reference to
    // nothing the dialog deletes first, but the destructor still has to stop it
    // before the widgets its signals update are gone. Constructing and
    // destroying repeatedly is what would surface a mistake there.
    std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate("regtest")};
    QVERIFY(style);

    for (int i{0}; i < 5; ++i) {
        HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
        Deliver(dialog, Result("4.22.9.0", "4.22.9.4", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz"));
        QCOMPARE(DownloadButtons(dialog), 1);
        // Destroyed here, with a worker created and its thread never started.
    }
    QVERIFY(true);
}
