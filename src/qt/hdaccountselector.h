// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_HDACCOUNTSELECTOR_H
#define BITCOIN_QT_HDACCOUNTSELECTOR_H

#include <QWidget>

#include <cstdint>

class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
QT_END_NAMESPACE

/** Chooses the HD account of a payment or of a new address. Shown only for a
 *  wallet that holds accounts; account 0 otherwise. */
class HDAccountSelector : public QWidget
{
    Q_OBJECT

public:
    enum class Purpose {
        //! The account a payment spends: active and retired accounts.
        Spend,
        //! The account of a new receive address: active accounts only.
        Receive,
    };

    HDAccountSelector(Purpose purpose, QWidget* parent = nullptr);

    void setModel(WalletModel* model);

    //! The account chosen.
    uint32_t account() const;
    //! Choose an account, if it is listed.
    void setAccount(uint32_t account);

    QComboBox* comboBox() const { return m_combo; }

public Q_SLOTS:
    //! Read the accounts again, keeping the choice if the account is still listed.
    void refresh();

Q_SIGNALS:
    void accountChanged(uint32_t account);

private:
    const Purpose m_purpose;
    WalletModel* m_model{nullptr};
    QLabel* m_label;
    QComboBox* m_combo;
    bool m_refreshing{false};
};

#endif // BITCOIN_QT_HDACCOUNTSELECTOR_H
