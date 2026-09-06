// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/utilitydialog.h>

#include <qt/forms/ui_helpmessagedialog.h>

#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/updatecheckworker.h>
#include <qt/updatedownloadworker.h>

#include <clientversion.h>
#include <init.h>
#include <util/system.h>
#include <util/strencodings.h>

#include <stdio.h>

#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QMainWindow>
#include <QRegExp>
#include <QTextCursor>
#include <QTextTable>
#include <QVBoxLayout>

/** "Help message" or "About" dialog box */
HelpMessageDialog::HelpMessageDialog(QWidget *parent, const NetworkStyle* networkStyle, bool about, bool checkUpdates, bool auto_check) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::HelpMessageDialog)
{
    ui->setupUi(this);

    QString version = QString{PACKAGE_NAME} + " " + tr("version") + " " + QString::fromStdString(FormatFullVersion());

    if (about || checkUpdates)
    {
        // Make URLs clickable
        QRegExp uri("<(.*)>", Qt::CaseSensitive, QRegExp::RegExp2);
        uri.setMinimal(true); // use non-greedy matching

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
        if(networkStyle) {
            const QSize requiredSize(1024,1024);
            QPixmap icon(networkStyle->getAppIcon().pixmap(requiredSize));
            ui->aboutLogo->setPixmap(icon);
        }

        if (about) {
            resize(780, 400);
            setWindowTitle(tr("About %1").arg(PACKAGE_NAME));

            std::string licenseInfo = LicenseInfo();
            /// HTML-format the license message from the core
            QString licenseInfoHTML = QString::fromStdString(LicenseInfo());
            licenseInfoHTML.replace(uri, "<a href=\"\\1\">\\1</a>");
            // Replace newlines with HTML breaks
            licenseInfoHTML.replace("\n", "<br>");

            text = version + "\n" + QString::fromStdString(FormatParagraph(licenseInfo));
            ui->aboutMessage->setText(version + "<br><br>" + licenseInfoHTML);

        } else {
            resize(780, 240);

            setWindowTitle(tr("Check for updates"));
            text = "Checking for updates. Please wait...";
            ui->aboutMessage->setText(text);

            // The node performs a network request for this, so it runs on a
            // worker thread and showUpdateInfo() fills the dialog in once the
            // answer arrives. Until then the "please wait" text above stands.
            if (auto_check) startUpdateCheck();
        }


    } else {
        resize(780, 400);
        setWindowTitle(tr("Command-line options"));
        QString header = "Usage:  reddcoin-qt [command-line options]                     \n";
        QTextCursor cursor(ui->helpMessage->document());
        cursor.insertText(version);
        cursor.insertBlock();
        cursor.insertText(header);
        cursor.insertBlock();

        std::string strUsage = gArgs.GetHelpMessage();
        QString coreOptions = QString::fromStdString(strUsage);
        text = version + "\n\n" + header + "\n" + coreOptions;

        QTextTableFormat tf;
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_None);
        tf.setCellPadding(2);
        QVector<QTextLength> widths;
        widths << QTextLength(QTextLength::PercentageLength, 35);
        widths << QTextLength(QTextLength::PercentageLength, 65);
        tf.setColumnWidthConstraints(widths);

        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);

        for (const QString &line : coreOptions.split("\n")) {
            if (line.startsWith("  -"))
            {
                cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::PreviousCell);
                cursor.movePosition(QTextCursor::NextRow);
                cursor.insertText(line.trimmed());
                cursor.movePosition(QTextCursor::NextCell);
            } else if (line.startsWith("   ")) {
                cursor.insertText(line.trimmed()+' ');
            } else if (line.size() > 0) {
                //Title of a group
                if (cursor.currentTable())
                    cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::Down);
                cursor.insertText(line.trimmed(), bold);
                cursor.insertTable(1, 2, tf);
            }
        }

        ui->helpMessage->moveCursor(QTextCursor::Start);
        ui->scrollArea->setVisible(false);
        ui->aboutLogo->setVisible(false);
    }

    GUIUtil::handleCloseWindowShortcut(this);
}

void HelpMessageDialog::startUpdateCheck()
{
    UpdateCheckWorker* worker = new UpdateCheckWorker();
    worker->moveToThread(&m_update_check_thread);

    connect(worker, &UpdateCheckWorker::checked, this, &HelpMessageDialog::showUpdateInfo);
    connect(&m_update_check_thread, &QThread::finished, worker, &UpdateCheckWorker::deleteLater);
    connect(&m_update_check_thread, &QThread::started, worker, &UpdateCheckWorker::check);

    m_update_check_thread.start();
}

void HelpMessageDialog::addDownloadControls(const QString& version, const QString& artifact)
{
    // Built here rather than in the .ui file because they exist only on the
    // update path, and only when there is a named artifact to fetch.
    m_download_status = new QLabel{this};
    m_download_status->setWordWrap(true);
    m_download_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_download_progress = new QProgressBar{this};
    m_download_progress->setRange(0, 100);
    m_download_progress->setValue(0);
    m_download_progress->setVisible(false);

    m_download_button = new QPushButton{tr("Download and verify"), this};
    m_download_button->setToolTip(
        tr("Download %1 and check it against the signing key built into this client. "
           "Nothing is installed.").arg(artifact));

    if (QLayout* layout = this->layout()) {
        layout->addWidget(m_download_status);
        layout->addWidget(m_download_progress);
        layout->addWidget(m_download_button);
    }

    m_download_worker = new UpdateDownloadWorker{version, artifact};
    m_download_worker->moveToThread(&m_download_thread);

    connect(&m_download_thread, &QThread::finished, m_download_worker, &QObject::deleteLater);
    connect(&m_download_thread, &QThread::started, m_download_worker, &UpdateDownloadWorker::download);
    connect(m_download_worker, &UpdateDownloadWorker::progressed,
            this, &HelpMessageDialog::onDownloadProgress);
    connect(m_download_worker, &UpdateDownloadWorker::finished,
            this, &HelpMessageDialog::onDownloadFinished);
    connect(m_download_button, &QPushButton::clicked, this, &HelpMessageDialog::onDownloadClicked);
}

void HelpMessageDialog::onDownloadClicked()
{
    if (m_download_thread.isRunning()) {
        // Second click cancels. The worker checks the flag between reads, so
        // the transfer stops at the next one rather than being torn down from
        // under the thread performing it.
        m_download_button->setEnabled(false);
        m_download_button->setText(tr("Cancelling..."));
        if (m_download_worker) m_download_worker->cancel();
        return;
    }

    m_download_progress->setValue(0);
    m_download_progress->setVisible(true);
    m_download_status->setText(tr("Downloading..."));
    m_download_button->setText(tr("Cancel"));
    m_download_thread.start();
}

void HelpMessageDialog::onDownloadProgress(qint64 received, qint64 total)
{
    if (total > 0) {
        m_download_progress->setRange(0, 100);
        m_download_progress->setValue(static_cast<int>((received * 100) / total));
        m_download_status->setText(tr("Downloading... %1 of %2 MB")
                                       .arg(received / (1024 * 1024))
                                       .arg(total / (1024 * 1024)));
        return;
    }

    // No declared length, so there is no honest percentage to show. A busy
    // indicator says "working" without inventing a position.
    m_download_progress->setRange(0, 0);
    m_download_status->setText(tr("Downloading... %1 MB so far").arg(received / (1024 * 1024)));
}

void HelpMessageDialog::onDownloadFinished(bool ok, const QString& path, qint64 size,
                                           const QString& error)
{
    m_download_thread.quit();
    m_download_progress->setVisible(false);
    m_download_button->setEnabled(true);
    m_download_button->setText(tr("Download and verify"));

    if (ok) {
        // Say what was actually established. "Downloaded" would undersell it
        // and "installed" would be untrue.
        m_download_status->setText(
            tr("Verified against the release signing key (%1 MB).<br>Saved to: %2")
                .arg(size / (1024 * 1024))
                .arg(path.toHtmlEscaped()));

        // Hand it to the platform rather than acting on it. The client does not
        // install anything and does not replace its own files, so the last step
        // is always the user's.
        m_downloaded_path = path;
        m_download_button->setText(handOffLabel());
        m_download_button->setToolTip(handOffTooltip());
        disconnect(m_download_button, &QPushButton::clicked, this,
                   &HelpMessageDialog::onDownloadClicked);
        connect(m_download_button, &QPushButton::clicked, this,
                &HelpMessageDialog::onHandOffClicked);
        return;
    }

    if (error.isEmpty()) {
        m_download_status->setText(tr("Download cancelled."));
        return;
    }
    m_download_status->setText("<font color='red'>" + error.toHtmlEscaped() + "</font>");
}

QString HelpMessageDialog::handOffLabel()
{
#if defined(Q_OS_WIN)
    return tr("Run installer");
#elif defined(Q_OS_MACOS)
    return tr("Open disk image");
#else
    return tr("Show in folder");
#endif
}

QString HelpMessageDialog::handOffTooltip()
{
#if defined(Q_OS_WIN)
    return tr("Start the installer. %1 will not close itself; quit it before installing.")
        .arg(PACKAGE_NAME);
#elif defined(Q_OS_MACOS)
    return tr("Open the disk image so the application can be dragged into Applications. "
              "%1 will not close itself; quit it before replacing it.").arg(PACKAGE_NAME);
#else
    return tr("Open the folder containing the verified archive.");
#endif
}

void HelpMessageDialog::onHandOffClicked()
{
    if (m_downloaded_path.isEmpty()) return;

    // On Linux the install shape is not knowable from here. A tarball, a distro
    // package, a Snap, a Flatpak and a container are all plausible and nothing
    // in the client can tell which one this user is running, so opening the
    // containing folder is the honest end of the sequence. Opening the archive
    // itself would hand it to an archive manager, which is a guess.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const QUrl target{QUrl::fromLocalFile(m_downloaded_path)};
#else
    const QUrl target{QUrl::fromLocalFile(QFileInfo{m_downloaded_path}.absolutePath())};
#endif

    if (!QDesktopServices::openUrl(target)) {
        // Do not leave the user with a button that silently does nothing. The
        // file is verified and its path is already on screen above.
        m_download_status->setText(
            m_download_status->text() + "<br><font color='red'>" +
            tr("Could not open it. The file is at the path above.").toHtmlEscaped() + "</font>");
    }
}

void HelpMessageDialog::showUpdateInfo(const QVariantMap& info)
{
    const QString localversion{info.value("localversion").toString()};
    const QString remoteversion{info.value("remoteversion").toString()};
    const QString errors{info.value("errors").toString()};
    const QString warning{info.value("warning").toString()};

    if (!errors.isEmpty()) {
        text = "<font color = 'red'>Error: </font>";
        text += errors;
    } else if (localversion == remoteversion) {
        text = "Installed version: <b>" + localversion + "</b><br>";
        text += info.value("message").toString();
    } else {
        const QString artifact{info.value("guiartifact").toString()};
        const QString artifact_link{info.value("guiartifactlink").toString()};

        text = "Installed version: <b>" + localversion + "</b><br>";
        text += "Latest repository version: <b>" + remoteversion + "</b><br><br>";

        if (artifact.isEmpty() || artifact_link.isEmpty()) {
            // Either no build is published for this host, or the release is a
            // prerelease, whose artifact naming has never been exercised. Point
            // at the directory and let the user choose, rather than name a file
            // that may not be there.
            const QString link{info.value("officialDownloadLink").toString()};
            const QString url{"<a href=\"" + link + "\">" + link + "</a>"};
            text += "Please download the latest version from our official website <br>(" + url + ").";
        } else {
            // The build this machine needs, rather than the directory holding
            // fifteen files it would have to choose between.
            text += "The build for this machine is:<br>";
            text += "<a href=\"" + artifact_link + "\">" + artifact + "</a>";

            addDownloadControls(remoteversion, artifact);

            if (info.value("platform").toString() == "osx64") {
                // Only an x86_64 macOS build is published, and it runs on Apple
                // Silicon under Rosetta. Say which one it is rather than let an
                // arm64 user assume it is native.
                text += "<br><br>This is the Intel build. It runs on Apple Silicon under Rosetta.";
            }
        }
    }

    // The pre-release caution belongs on every outcome, not just one branch: it
    // describes the build the user is running rather than anything the check
    // discovered, so it is appended to whatever the text above ended up being.
    if (!warning.isEmpty()) {
        if (!text.isEmpty()) {
            text += "<br><br>";
        }
        text += "<font color = 'red'>" + warning + "</font>";
    }

    ui->aboutMessage->setText(text);
}

HelpMessageDialog::~HelpMessageDialog()
{
    // An in flight request must not outlive the widgets its reply updates.
    m_update_check_thread.quit();
    m_update_check_thread.wait();

    // A download can be minutes from finishing, so ask it to stop before
    // waiting rather than blocking the close on a transfer nobody wants any
    // more. quit() alone would not do it: the worker is inside a blocking call
    // and is not reading the event loop, so it has to be told through the flag
    // it polls between reads.
    if (m_download_worker) m_download_worker->cancel();
    m_download_thread.quit();
    m_download_thread.wait();

    delete ui;
}

void HelpMessageDialog::printToConsole()
{
    // On other operating systems, the expected action is to print the message to the console.
    tfm::format(std::cout, "%s\n", qPrintable(text));
}

void HelpMessageDialog::showOrPrint()
{
#if defined(WIN32)
    // On Windows, show a message box, as there is no stderr/stdout in windowed applications
    exec();
#else
    // On other operating systems, print help text to console
    printToConsole();
#endif
}

void HelpMessageDialog::on_okButton_accepted()
{
    close();
}


/** "Shutdown" window */
ShutdownWindow::ShutdownWindow(QWidget *parent, Qt::WindowFlags f):
    QWidget(parent, f)
{
    QVBoxLayout *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(
        tr("%1 is shutting down…").arg(PACKAGE_NAME) + "<br /><br />" +
        tr("Do not shut down the computer until this window disappears.")));
    setLayout(layout);

    GUIUtil::handleCloseWindowShortcut(this);
}

QWidget* ShutdownWindow::showShutdownWindow(QMainWindow* window)
{
    assert(window != nullptr);

    // Show a simple window indicating shutdown status
    QWidget *shutdownWindow = new ShutdownWindow();
    shutdownWindow->setWindowTitle(window->windowTitle());

    // Center shutdown window at where main window was
    const QPoint global = window->mapToGlobal(window->rect().center());
    shutdownWindow->move(global.x() - shutdownWindow->width() / 2, global.y() - shutdownWindow->height() / 2);
    shutdownWindow->show();
    return shutdownWindow;
}

void ShutdownWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
}
