// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_UTILITYDIALOG_H
#define BITCOIN_QT_UTILITYDIALOG_H

#include <QDialog>
#include <QThread>
#include <QWidget>

class NetworkStyle;
class UpdateDownloadWorker;

QT_BEGIN_NAMESPACE
class QLabel;
class QProgressBar;
class QPushButton;
QT_END_NAMESPACE

QT_BEGIN_NAMESPACE
class QMainWindow;
QT_END_NAMESPACE

namespace Ui {
    class HelpMessageDialog;
}

/** "Help message" dialog box */
class HelpMessageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HelpMessageDialog(QWidget *parent, const NetworkStyle *networkStyle, bool about, bool checkUpdates);
    ~HelpMessageDialog();

    void printToConsole();
    void showOrPrint();

private:
    /** Add the download button and progress bar, once there is something to offer */
    void addDownloadControls(const QString& version, const QString& artifact);

    Ui::HelpMessageDialog *ui;
    QString text;

    /** Thread the download runs on, so a 29 MB transfer cannot stall the GUI */
    QThread m_download_thread;
    /** Owned by m_download_thread, which deletes it when it finishes */
    UpdateDownloadWorker* m_download_worker{nullptr};
    QPushButton* m_download_button{nullptr};
    QProgressBar* m_download_progress{nullptr};
    QLabel* m_download_status{nullptr};

private Q_SLOTS:
    void on_okButton_accepted();

    /** Start the download, or ask a running one to stop */
    void onDownloadClicked();
    /** Move the bar. Queued from the worker thread, already throttled there */
    void onDownloadProgress(qint64 received, qint64 total);
    /** Report where the verified file is, or why there is not one */
    void onDownloadFinished(bool ok, const QString& path, qint64 size, const QString& error);
};


/** "Shutdown" window */
class ShutdownWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ShutdownWindow(QWidget *parent=nullptr, Qt::WindowFlags f=Qt::Widget);
    static QWidget* showShutdownWindow(QMainWindow* window);

protected:
    void closeEvent(QCloseEvent *event) override;
};

#endif // BITCOIN_QT_UTILITYDIALOG_H
