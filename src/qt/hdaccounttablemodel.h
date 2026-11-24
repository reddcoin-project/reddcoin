// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_HDACCOUNTTABLEMODEL_H
#define BITCOIN_QT_HDACCOUNTTABLEMODEL_H

#include <interfaces/wallet.h>

#include <QAbstractTableModel>
#include <QStringList>

#include <vector>

class WalletModel;

/** The HD accounts of a legacy wallet: account 0 and the accounts handed
 *  out, active and retired, with what each holds. Read on refresh() only:
 *  reading them takes a pass over the wallet's transactions. */
class HDAccountTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    HDAccountTableModel(WalletModel* wallet_model, QObject* parent);

    enum ColumnIndex {
        Number = 0,
        Label,
        State,
        Available,
        Pending,
        Immature,
        Used,
    };

    enum RoleIndex {
        /** The account number, uint */
        NumberRole = Qt::UserRole,
        /** Whether the account is retired, bool */
        RetiredRole,
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    //! The account of a row.
    const interfaces::WalletHDAccount* account(int row) const;

public Q_SLOTS:
    //! Read the accounts from the wallet again.
    void refresh();

private:
    WalletModel* const m_wallet_model;
    std::vector<interfaces::WalletHDAccount> m_accounts;
    const QStringList m_columns;
};

#endif // BITCOIN_QT_HDACCOUNTTABLEMODEL_H
