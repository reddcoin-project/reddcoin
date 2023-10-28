// Copyright (c) 2014-2023 The Reddcoin Core developers
// Copyright (c) 2012-2021 The Peercoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MINTINGVIEW_H
#define BITCOIN_QT_MINTINGVIEW_H

#include <QWidget>
#include <QComboBox>
#include <qt/mintingfilterproxy.h>

class PlatformStyle;
class WalletModel;


QT_BEGIN_NAMESPACE
class QTableView;
class QMenu;
QT_END_NAMESPACE

class MintingView : public QWidget
{
    Q_OBJECT
public:
    explicit MintingView(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    void setModel(WalletModel *model);

    enum MintingEnum
    {
        Minting1min,
        Minting10min,
        Minting1hour,
        Minting1day,
        Minting30days,
        Minting90days
    };

protected:
    void changeEvent(QEvent* e) override;

private:
    WalletModel *model{nullptr};
    MintingFilterProxy *mintingProxyModel{nullptr};
    QTableView *mintingView{nullptr};
    QComboBox *mintingCombo;
    QMenu *contextMenu;

    const PlatformStyle* m_platform_style;

private Q_SLOTS:
    void contextualMenu(const QPoint &);
    void copyAddress();
    void copyTransactionId();

Q_SIGNALS:

public Q_SLOTS:
    void exportClicked();
    void chooseMintingInterval(int idx);
};

#endif // BITCOIN_QT_MINTINGVIEW_H
