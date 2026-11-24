// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/hdaccounttablemodel.h>

#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

HDAccountTableModel::HDAccountTableModel(WalletModel* wallet_model, QObject* parent)
    : QAbstractTableModel(parent),
      m_wallet_model(wallet_model),
      m_columns{tr("Account"), tr("Label"), tr("State"), tr("Available"), tr("Pending"), tr("Immature"), tr("Used")}
{
    if (OptionsModel* options = m_wallet_model->getOptionsModel()) {
        connect(options, &OptionsModel::displayUnitChanged, this, [this] {
            if (!m_accounts.empty()) Q_EMIT dataChanged(index(0, Available), index(rowCount() - 1, Immature));
        });
    }
}

void HDAccountTableModel::refresh()
{
    std::vector<interfaces::WalletHDAccount> accounts{m_wallet_model->wallet().listHDAccounts(/* with_balances */ true)};
    beginResetModel();
    m_accounts = std::move(accounts);
    endResetModel();
}

int HDAccountTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_accounts.size());
}

int HDAccountTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_columns.size();
}

const interfaces::WalletHDAccount* HDAccountTableModel::account(int row) const
{
    if (row < 0 || row >= rowCount()) return nullptr;
    return &m_accounts[row];
}

QVariant HDAccountTableModel::data(const QModelIndex& index, int role) const
{
    const interfaces::WalletHDAccount* entry = index.isValid() ? account(index.row()) : nullptr;
    if (!entry) return {};
    if (role == NumberRole) return entry->number;
    if (role == RetiredRole) return entry->retired;
    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case Number:
        case Available:
        case Pending:
        case Immature:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};

    const int unit{m_wallet_model->getOptionsModel() ? m_wallet_model->getOptionsModel()->getDisplayUnit() : BitcoinUnits::BTC};
    switch (index.column()) {
    case Number:
        return entry->number;
    case Label:
        if (entry->number == 0 && role == Qt::DisplayRole) return tr("(the wallet's own)");
        return QString::fromStdString(entry->label);
    case State:
        return entry->retired ? tr("Retired") : tr("Active");
    case Available:
        return role == Qt::EditRole ? QVariant(qint64(entry->trusted)) : QVariant(BitcoinUnits::format(unit, entry->trusted, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    case Pending:
        return role == Qt::EditRole ? QVariant(qint64(entry->untrusted_pending)) : QVariant(BitcoinUnits::format(unit, entry->untrusted_pending, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    case Immature:
        return role == Qt::EditRole ? QVariant(qint64(entry->immature)) : QVariant(BitcoinUnits::format(unit, entry->immature, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    case Used:
        return entry->used ? tr("Yes") : tr("No");
    }
    return {};
}

QVariant HDAccountTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= m_columns.size()) return {};
    if (role == Qt::DisplayRole) return m_columns[section];
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Number:
            return tr("The number of the account. A restore finds accounts by number, not by label.");
        case State:
            return tr("A retired account gives no new address; its coins are still watched and spendable.");
        case Available:
            return tr("What the account can spend now.");
        case Pending:
            return tr("Payments to the account not yet confirmed.");
        case Immature:
            return tr("Stakes of the account that are not yet mature.");
        case Used:
            return tr("Whether a transaction of the wallet pays the account.");
        }
    }
    return {};
}
