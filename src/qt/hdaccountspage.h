// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_HDACCOUNTSPAGE_H
#define BITCOIN_QT_HDACCOUNTSPAGE_H

#include <qt/walletmodel.h>

#include <QWidget>

#include <memory>
#include <thread>

class HDAccountTableModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTableView;
QT_END_NAMESPACE

/** The HD accounts of a legacy HD wallet: what each holds, and creating,
 *  renaming, retiring and finding them. */
class HDAccountsPage : public QWidget
{
    Q_OBJECT

public:
    explicit HDAccountsPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);
    ~HDAccountsPage();

    void setModel(WalletModel* model);

    //! Whether a scan for accounts is running.
    bool isScanning() const { return m_scan_thread.joinable(); }

    // The actions, also for the tests. Each asks what it needs to.
    QPushButton* newAccountButton() const { return m_new_button; }
    QPushButton* renameButton() const { return m_rename_button; }
    QPushButton* retireButton() const { return m_retire_button; }
    QPushButton* unpaidButton() const { return m_unpaid_button; }
    QPushButton* findButton() const { return m_find_button; }
    QTableView* tableView() const { return m_view; }
    HDAccountTableModel* tableModel() const { return m_table_model; }
    QLabel* noticeLabel() const { return m_notice; }
    QLabel* newAccountRefusalLabel() const { return m_new_refusal; }

public Q_SLOTS:
    //! Read the accounts and the state of the wallet again.
    void refresh();

    void newAccount();
    void renameAccount();
    void retireAccount();
    void showUnpaidAddresses();
    void findAccounts();

Q_SIGNALS:
    void message(const QString& title, const QString& message, unsigned int style);

protected:
    void showEvent(QShowEvent* event) override;

private:
    //! The account of the selected row, if a row is selected.
    const interfaces::WalletHDAccount* selectedAccount() const;
    void updateButtons();
    //! Stop a running scan and wait for its thread.
    void stopScan();
    void scanFinished(bool ok, const std::vector<uint32_t>& found, const QString& error);

    const PlatformStyle* const m_platform_style;
    WalletModel* m_model{nullptr};
    HDAccountTableModel* m_table_model{nullptr};
    interfaces::WalletHDAccountsState m_state;

    QLabel* m_intro;
    QLabel* m_notice;
    QTableView* m_view;
    QPushButton* m_new_button;
    QPushButton* m_rename_button;
    QPushButton* m_retire_button;
    QPushButton* m_unpaid_button;
    QPushButton* m_find_button;
    QLabel* m_new_refusal;
    QLabel* m_earlier_seed;

    std::thread m_scan_thread;
    //! Keeps the wallet unlocked while the scan runs.
    std::unique_ptr<WalletModel::UnlockContext> m_scan_unlock;
};

#endif // BITCOIN_QT_HDACCOUNTSPAGE_H
