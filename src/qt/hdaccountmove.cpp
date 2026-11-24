// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/hdaccountmove.h>

#include <key_io.h>
#include <outputtype.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>

#include <QMessageBox>
#include <QStringList>
#include <QWidget>

#include <memory>
#include <optional>

std::vector<HDAccountMove> PlanHDAccountMove(const std::vector<interfaces::WalletHDAccount>& accounts)
{
    std::vector<HDAccountMove> plan;
    for (const interfaces::WalletHDAccount& account : accounts) {
        if (account.trusted <= 0) continue;
        HDAccountMove move;
        move.from = account.number;
        move.label = account.label;
        move.amount = account.trusted;
        move.to = static_cast<uint32_t>(plan.size());
        plan.push_back(move);
    }
    return plan;
}

bool MovesHDAccounts(const std::vector<HDAccountMove>& plan)
{
    for (const HDAccountMove& move : plan) {
        if (move.from != 0 || move.to != 0) return true;
    }
    return false;
}

namespace {
QString AccountName(uint32_t number, const std::string& label)
{
    return label.empty() ? QObject::tr("account %1").arg(number) : QObject::tr("account %1 (%2)").arg(number).arg(QString::fromStdString(label));
}
} // namespace

QString DescribeHDAccountMove(const std::vector<HDAccountMove>& plan, int unit)
{
    QStringList lines;
    for (const HDAccountMove& move : plan) {
        lines << QObject::tr("%1 of %2 to account %3 of the new wallet").arg(BitcoinUnits::formatWithUnit(unit, move.amount), AccountName(move.from, move.label)).arg(move.to);
    }
    return lines.join("\n");
}

bool MoveHDAccounts(WalletModel& from, WalletModel& to, const std::vector<HDAccountMove>& plan, QWidget* parent, QString& result)
{
    const int unit{from.getOptionsModel() ? from.getOptionsModel()->getDisplayUnit() : BitcoinUnits::BTC};
    const QString from_name{from.getDisplayName()};

    // The accounts of the new wallet first, all of them: each is paid below,
    // so BIP44's account rule holds once the payments confirm.
    std::optional<WalletModel::UnlockContext> to_unlock;
    if (to.getEncryptionStatus() == WalletModel::Locked) {
        to_unlock.emplace(to.requestUnlock());
        if (!to_unlock->isValid()) {
            result = QObject::tr("The new wallet %1 was not unlocked: nothing was moved.").arg(to.getDisplayName());
            return false;
        }
    }
    OutputType type{to.wallet().getDefaultAddressType()};
    if (type == OutputType::BECH32M) type = OutputType::BECH32;
    std::vector<CTxDestination> addresses;
    for (const HDAccountMove& move : plan) {
        const std::string label{QObject::tr("From wallet %1, %2").arg(from_name, AccountName(move.from, move.label)).toStdString()};
        CTxDestination dest;
        if (move.to == 0) {
            if (!to.wallet().getNewDestination(type, label, dest)) {
                result = QObject::tr("The new wallet %1 could not give an address: nothing was moved.").arg(to.getDisplayName());
                return false;
            }
        } else {
            uint32_t account{0};
            std::vector<bilingual_str> warnings;
            bilingual_str error;
            if (!to.wallet().newHDAccount(move.label, /* force */ true, account, dest, warnings, error) || account != move.to) {
                result = QObject::tr("Account %1 of the new wallet %2 could not be created: nothing was moved.").arg(move.to).arg(to.getDisplayName());
                if (!error.empty()) result += " " + QString::fromStdString(error.translated);
                return false;
            }
            to.wallet().setAddressBook(dest, label, "receive");
        }
        addresses.push_back(dest);
    }
    if (MovesHDAccounts(plan)) Q_EMIT to.hdAccountsChanged();

    // The payments, made before any is sent, so that one that cannot be
    // made sends nothing.
    std::optional<WalletModel::UnlockContext> from_unlock;
    if (from.getEncryptionStatus() == WalletModel::Locked) {
        from_unlock.emplace(from.requestUnlock());
        if (!from_unlock->isValid()) {
            result = QObject::tr("Wallet %1 was not unlocked: nothing was moved. The accounts of the new wallet are created.").arg(from_name);
            return false;
        }
    }
    std::vector<std::unique_ptr<WalletModelTransaction>> transactions;
    QStringList lines;
    for (size_t i = 0; i < plan.size(); ++i) {
        const HDAccountMove& move = plan[i];
        CCoinControl coin_control;
        coin_control.m_hd_account = move.from;
        SendCoinsRecipient recipient;
        recipient.address = QString::fromStdString(EncodeDestination(addresses[i]));
        recipient.label = QObject::tr("To wallet %1, account %2").arg(to.getDisplayName()).arg(move.to);
        recipient.amount = from.wallet().getAvailableBalance(coin_control);
        recipient.fSubtractFeeFromAmount = true;
        auto transaction = std::make_unique<WalletModelTransaction>(QList<SendCoinsRecipient>{recipient});
        if (recipient.amount <= 0 || from.prepareTransaction(*transaction, coin_control).status != WalletModel::OK) {
            result = QObject::tr("The payment of %1 could not be made: nothing was moved. The accounts of the new wallet are created.").arg(AccountName(move.from, move.label));
            return false;
        }
        lines << QObject::tr("%1 of %2 to account %3 of the new wallet, fee %4").arg(
            BitcoinUnits::formatWithUnit(unit, transaction->getTotalTransactionAmount()), AccountName(move.from, move.label)).arg(move.to).arg(
            BitcoinUnits::formatWithUnit(unit, transaction->getTransactionFee()));
        transactions.push_back(std::move(transaction));
    }
    if (QMessageBox::question(parent, QObject::tr("Move funds to a new wallet"),
            QObject::tr("Each HD account moves in its own transaction, to an account of the new wallet %1, so that its phrase restores every account:").arg(to.getDisplayName()) +
                "\n\n" + lines.join("\n") + "\n\n" + QObject::tr("Send these %n transaction(s)?", "", static_cast<int>(transactions.size())),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        result = QObject::tr("Nothing was moved. The accounts of the new wallet are created.");
        return false;
    }

    // In order, so that no account is left paid behind one that is not.
    QStringList sent;
    for (size_t i = 0; i < transactions.size(); ++i) {
        const HDAccountMove& move = plan[i];
        from.sendCoins(*transactions[i]);
        const uint256 txid{transactions[i]->getWtx()->GetHash()};
        interfaces::WalletTxStatus status;
        interfaces::WalletOrderForm order_form;
        bool in_mempool{false};
        int num_blocks{0};
        from.wallet().getWalletTxDetails(txid, status, order_form, in_mempool, num_blocks);
        if (!in_mempool && status.depth_in_main_chain < 1) {
            result = (sent.isEmpty() ? QString() : QObject::tr("Moved:") + "\n" + sent.join("\n") + "\n\n") +
                     QObject::tr("The payment of %1 (transaction %2) is not in the memory pool: the node refused it, or does not broadcast wallet transactions (-walletbroadcast=0). The accounts after it were not moved; move them once it is resolved.").arg(AccountName(move.from, move.label), QString::fromStdString(txid.GetHex()));
            return false;
        }
        sent << QObject::tr("%1 to account %2 of the new wallet, transaction %3").arg(AccountName(move.from, move.label)).arg(move.to).arg(QString::fromStdString(txid.GetHex()));
    }
    result = QObject::tr("Moved:") + "\n" + sent.join("\n") + "\n\n" +
             QObject::tr("Keep the file of wallet %1 until the payments are confirmed.").arg(from_name);
    return true;
}
