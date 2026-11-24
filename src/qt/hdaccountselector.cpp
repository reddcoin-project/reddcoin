// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/hdaccountselector.h>

#include <interfaces/wallet.h>
#include <qt/walletmodel.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>

HDAccountSelector::HDAccountSelector(Purpose purpose, QWidget* parent)
    : QWidget(parent),
      m_purpose(purpose),
      m_label(new QLabel(purpose == Purpose::Spend ? tr("Spend from:") : tr("HD account:"), this)),
      m_combo(new QComboBox(this))
{
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);
    layout->addWidget(m_combo);
    layout->addStretch();
    m_label->setBuddy(m_combo);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_combo->setToolTip(purpose == Purpose::Spend ?
        tr("The HD account this payment spends. Its change goes back to it; coins of two accounts are never spent together.") :
        tr("The HD account the new address belongs to."));
    connect(m_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_refreshing) Q_EMIT accountChanged(account());
    });
    setVisible(false);
}

void HDAccountSelector::setModel(WalletModel* model)
{
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (m_model) {
        connect(m_model, &WalletModel::hdAccountsChanged, this, &HDAccountSelector::refresh);
        connect(m_model, &WalletModel::balanceChanged, this, &HDAccountSelector::refresh);
    }
    refresh();
}

uint32_t HDAccountSelector::account() const
{
    return m_combo->currentIndex() < 0 ? 0 : m_combo->currentData().toUInt();
}

void HDAccountSelector::setAccount(uint32_t account)
{
    const int index{m_combo->findData(account)};
    if (index >= 0) m_combo->setCurrentIndex(index);
}

void HDAccountSelector::refresh()
{
    const uint32_t before{account()};
    // Account 0 alone: a wallet without accounts, or of a kind without them.
    std::vector<interfaces::WalletHDAccount> accounts;
    if (m_model) accounts = m_model->wallet().listHDAccounts(/* with_balances */ false);
    const bool shown{accounts.size() > 1};

    m_refreshing = true;
    m_combo->clear();
    for (const interfaces::WalletHDAccount& entry : accounts) {
        if (entry.retired && m_purpose == Purpose::Receive) continue;
        QString text{entry.label.empty() ? tr("Account %1").arg(entry.number) : tr("Account %1 (%2)").arg(entry.number).arg(QString::fromStdString(entry.label))};
        if (entry.retired) text = tr("%1, retired").arg(text);
        m_combo->addItem(text, entry.number);
    }
    if (m_combo->count() == 0) m_combo->addItem(tr("Account 0"), 0u);
    int index{m_combo->findData(before)};
    if (index < 0) index = 0;
    m_combo->setCurrentIndex(index);
    m_refreshing = false;
    setVisible(shown);
    // The account listed before is gone (retired, for a receive address): the choice is now another.
    if (account() != before) Q_EMIT accountChanged(account());
}
