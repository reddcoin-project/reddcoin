// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/updatedialogtests.h>

#include <qt/networkstyle.h>
#include <qt/updatecheckworker.h>
#include <qt/utilitydialog.h>

#include <univalue.h>

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

void UpdateDialogTests::everyFieldTheDialogReadsSurvivesTheWorker()
{
    // The contract the other tests cannot see. They hand the dialog a map built
    // in this file, which proves the dialog reads a map correctly and proves
    // nothing about whether the worker ever puts those keys in one.
    //
    // It did not. The worker named seven fields while the dialog had grown to
    // read ten, so guiartifact arrived empty and the dialog silently took its
    // "no artifact published" branch on every update. The named-file notice
    // phase 1 added never appeared through the real path, and no test noticed,
    // because every test supplied its own map.
    UniValue result{UniValue::VOBJ};
    result.pushKV("localversion", "4.22.9.0");
    result.pushKV("remoteversion", "4.22.9.4");
    result.pushKV("updateavailable", true);
    result.pushKV("message", "a message");
    result.pushKV("warning", "");
    result.pushKV("officialDownloadLink", "https://download.reddcoin.com/bin/reddcoin-core-4.22.9.4");
    result.pushKV("hosttriplet", "x86_64-pc-linux-gnu");
    result.pushKV("artifactbytes", 30603976);
    result.pushKV("platform", "x86_64-linux-gnu");
    result.pushKV("guiartifact", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz");
    result.pushKV("guiartifactlink", "https://download.reddcoin.com/x.tar.gz");
    result.pushKV("daemonartifact", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz");
    result.pushKV("daemonartifactlink", "https://download.reddcoin.com/x.tar.gz");
    result.pushKV("errors", "");

    const QVariantMap info{UpdateInfoToVariantMap(result)};

    // Nothing the node reports may be dropped on the way to the GUI.
    for (const std::string& key : result.getKeys()) {
        QVERIFY2(info.contains(QString::fromStdString(key)),
                 qPrintable(QString{"the worker dropped '%1'"}.arg(QString::fromStdString(key))));
    }

    QCOMPARE(info.value("guiartifact").toString(),
             QString{"reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz"});
    QCOMPARE(info.value("platform").toString(), QString{"x86_64-linux-gnu"});
    QCOMPARE(info.value("updateavailable").toBool(), true);
    QCOMPARE(info.value("artifactbytes").toLongLong(), 30603976LL);

    // And the dialog, fed what the worker actually produces rather than a map
    // written to suit it, offers the download.
    std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate("regtest")};
    QVERIFY(style);
    HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
    Deliver(dialog, info);
    QCOMPARE(DownloadButtons(dialog), 1);
}

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

void UpdateDialogTests::handOffWordingMatchesThePlatform()
{
    // The hand-off is the last step and it differs by platform, so what the
    // button says has to match what pressing it does. Getting this wrong means
    // telling a Linux user an installer will run when a file manager opens.
    std::unique_ptr<const NetworkStyle> style{NetworkStyle::instantiate("regtest")};
    QVERIFY(style);
    HelpMessageDialog dialog{nullptr, style.get(), false, true, nullptr};
    Deliver(dialog, Result("4.22.9.0", "4.22.9.4", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz"));

    QPushButton* button{nullptr};
    for (QPushButton* candidate : dialog.findChildren<QPushButton*>()) {
        if (candidate->text().contains("Download")) button = candidate;
    }
    QVERIFY(button);

    // Before a download there is nothing to hand off, so the button still
    // offers the download rather than an action with no file behind it.
    QVERIFY(button->text().contains("Download"));

    // The wording after a successful download is the platform's, and on Linux
    // it must not promise to install anything: B5 says the install shape there
    // is not knowable at runtime, so the sequence ends at revealing the file.
    QMetaObject::invokeMethod(&dialog, "onDownloadFinished", Qt::DirectConnection,
                              Q_ARG(bool, true), Q_ARG(QString, "/tmp/artifact.tar.gz"),
                              Q_ARG(qint64, 30603976), Q_ARG(QString, QString{}));

#if defined(Q_OS_WIN)
    QCOMPARE(button->text(), QString{"Run installer"});
#elif defined(Q_OS_MACOS)
    QCOMPARE(button->text(), QString{"Open disk image"});
#else
    QCOMPARE(button->text(), QString{"Show in folder"});
    QVERIFY(!button->text().contains("install", Qt::CaseInsensitive));
#endif
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
