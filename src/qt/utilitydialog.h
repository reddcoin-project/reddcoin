// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_UTILITYDIALOG_H
#define BITCOIN_QT_UTILITYDIALOG_H

#include <QDialog>
#include <QThread>
#include <QVariantMap>
#include <QWidget>

class NetworkStyle;

namespace interfaces {
class Node;
} // namespace interfaces

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
    //! node is only needed when checkUpdates is set, since the update check is
    //! performed by the node rather than by the GUI.
    explicit HelpMessageDialog(QWidget *parent, const NetworkStyle *networkStyle, bool about, bool checkUpdates, interfaces::Node* node = nullptr);
    ~HelpMessageDialog();

    void printToConsole();
    void showOrPrint();

private:
    /** Create the update check worker and start the thread it runs on */
    void startUpdateCheck(interfaces::Node& node);

    Ui::HelpMessageDialog *ui;
    QString text;
    /** Thread the update check runs on, so its network request cannot stall the GUI */
    QThread m_update_check_thread;

private Q_SLOTS:
    void on_okButton_accepted();
    /** Replace the "please wait" text once the check has an answer */
    void showUpdateInfo(const QVariantMap& info);
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
