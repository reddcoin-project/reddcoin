// Copyright (c) 2014-2026 The Reddcoin Core developers
// Copyright (c) 2012-2021 The Peercoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/mintingtablemodel.h>

#include <interfaces/node.h>
#include <pos/kernelrecord.h>
#include <qt/transactiondesc.h>
#include <qt/transactionrecord.h>
#include <qt/walletmodel.h>
#include <qt/guiconstants.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/addresstablemodel.h>

#include <wallet/wallet.h>
#include <validation.h>
#include <chainparams.h>
#include <node/ui_interface.h>

#include <QColor>
#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <vector>

// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
        Qt::AlignLeft|Qt::AlignVCenter,
        Qt::AlignLeft|Qt::AlignVCenter,
        Qt::AlignRight|Qt::AlignVCenter,
        Qt::AlignRight|Qt::AlignVCenter,
        Qt::AlignRight|Qt::AlignVCenter,
        Qt::AlignRight|Qt::AlignVCenter
    };

// Comparison operator for sort/binary search of model tx list
struct TxLessThan
{
    bool operator()(const KernelRecord &a, const KernelRecord &b) const
    {
        return a.hash != b.hash ? a.hash < b.hash : a.idx < b.idx;
    }
    bool operator()(const KernelRecord &a, const uint256 &b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256 &a, const KernelRecord &b) const
    {
        return a < b.hash;
    }
};

// Queue notification for batching during wallet rescans
struct TransactionNotification2
{
    TransactionNotification2() {}
    explicit TransactionNotification2(uint256 _hash):
        hash(_hash) {}

    void invoke(QObject *ttm) const
    {
        QString strHash = QString::fromStdString(hash.GetHex());
        QMetaObject::invokeMethod(ttm, "updateTransaction", Qt::QueuedConnection,
                                  Q_ARG(QString, strHash));
    }
private:
    uint256 hash;
};

// Private implementation
class MintingTablePriv
{
public:
    MintingTablePriv(WalletModel *walletModel, MintingTableModel *parent):
            walletModel(walletModel),
            parent(parent)
    {
    }
    WalletModel *walletModel;
    MintingTableModel *parent;

    /* Local cache of wallet.
     * As it is in the same order as the CWallet, by definition
     * this is sorted by sha256.
     */
    QList<KernelRecord> cachedWallet;

    /** True when initial wallet data has been loaded. Read by the wallet's notification threads. */
    std::atomic<bool> m_loaded{false};

    /** True while a rescan is in progress */
    std::atomic<bool> m_loading{false};

    /** Queued notifications during load/rescan */
    std::vector<TransactionNotification2> vQueueNotifications;

    void NotifyTransactionChanged(const uint256& hash, ChangeType /* status */)
    {
        // Nothing to keep in step until the initial load has run. After that
        // rows are reconciled against the wallet, whatever the kind of change.
        if (!m_loaded) return;
        TransactionNotification2 notification(hash);
        if (m_loading) {
            vQueueNotifications.push_back(notification);
            return;
        }
        notification.invoke(parent);
    }

    void DispatchNotifications()
    {
        if (!m_loaded || m_loading) return;
        for (const auto& notification : vQueueNotifications) {
            notification.invoke(parent);
        }
        vQueueNotifications.clear();
    }

    /* Query entire wallet anew from core.
     * One pass over the wallet under a single lock, yielding unspent outputs only.
     */
    void refreshWallet()
    {
        // Accept notifications before reading the wallet, so that a change
        // landing after the read is queued behind it rather than lost.
        m_loaded = true;
        cachedWallet.clear();
        for (const auto& output : walletModel->wallet().getMintingOutputs()) {
            cachedWallet.append(KernelRecord(output));
        }
        std::sort(cachedWallet.begin(), cachedWallet.end(), TxLessThan());
    }

    /* Update our model of the wallet incrementally, to synchronize our model of the wallet
       with that of the core.

       Call with transaction that was added, removed or changed. Its rows, and
       those of the transactions it spends from, are brought in line with the
       wallet's unspent outputs: a spent output is one row removed, a new one
       is one row inserted.
     */
    void updateWallet(const uint256 &hash)
    {
        std::vector<uint256> txids;
        const std::vector<interfaces::WalletMintingOutput> outputs = walletModel->wallet().getMintingOutputs(hash, txids);

        for (const uint256& txid : txids) {
            // Outputs the wallet lists for this transaction, in output order
            std::vector<const interfaces::WalletMintingOutput*> wanted;
            for (const auto& output : outputs) {
                if (output.outpoint.hash == txid) wanted.push_back(&output);
            }

            // Walk them against the rows the model holds for it
            int row = std::lower_bound(cachedWallet.begin(), cachedWallet.end(), txid, TxLessThan()) - cachedWallet.begin();
            size_t next = 0;
            while (true) {
                const bool have_row = row < cachedWallet.size() && cachedWallet.at(row).hash == txid;
                const bool have_wanted = next < wanted.size();
                if (!have_row && !have_wanted) break;

                const int wanted_idx = have_wanted ? int(wanted[next]->outpoint.n) : 0;
                if (have_row && (!have_wanted || cachedWallet.at(row).idx < wanted_idx)) {
                    // Spent, or no longer shown -- remove the row
                    parent->beginRemoveRows(QModelIndex(), row, row);
                    cachedWallet.removeAt(row);
                    parent->endRemoveRows();
                } else if (!have_row || wanted_idx < cachedWallet.at(row).idx) {
                    // Added -- insert at the right position
                    parent->beginInsertRows(QModelIndex(), row, row);
                    cachedWallet.insert(row, KernelRecord(*wanted[next]));
                    parent->endInsertRows();
                    ++row;
                    ++next;
                } else {
                    ++row;
                    ++next;
                }
            }
        }
    }

    void invalidateCaches()
    {
        for (auto& rec : cachedWallet) {
            rec.invalidateCache();
        }
    }

    int size()
    {
        return cachedWallet.size();
    }

    KernelRecord *index(int idx)
    {
        if(idx >= 0 && idx < cachedWallet.size())
        {
            KernelRecord *rec = &cachedWallet[idx];
            return rec;
        }
        else
        {
            return nullptr;
        }
    }

};

MintingTableModel::MintingTableModel(WalletModel *parent) :
        QAbstractTableModel(parent),
        walletModel(parent),
        mintingInterval(60),
        priv(new MintingTablePriv(walletModel, this))
{
    columns << tr("Transaction") <<  tr("Address") << tr("Age") << tr("Balance") << tr("Coin Day") << tr("Stake Probability");

    // Defer initial wallet load if we're in IBD — refreshWallet() acquires
    // LOCK(cs_wallet) which would block the GUI thread during sync.
    // The updateAge timer will trigger the load once IBD completes.
    if (!walletModel->node().isInitialBlockDownload()) {
        priv->refreshWallet();
        m_cached_difficulty = walletModel->node().getDifficulty();
    }

    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MintingTableModel::updateAge);
    timer->start(MINTING_UPDATE_DELAY);

    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &MintingTableModel::updateDisplayUnit);
    m_handler_transaction_changed = walletModel->wallet().handleTransactionChanged(
        std::bind(&MintingTablePriv::NotifyTransactionChanged, priv,
                  std::placeholders::_1, std::placeholders::_2));
    m_handler_show_progress = walletModel->wallet().handleShowProgress(
        [this](const std::string&, int progress) {
            priv->m_loading = progress < 100;
            priv->DispatchNotifications();
        });
}

MintingTableModel::~MintingTableModel()
{
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    delete priv;
}

void MintingTableModel::updateTransaction(const QString &hash)
{
    // The initial load is deferred past IBD; until it has run there are no
    // rows to keep in step.
    if (!priv->m_loaded) return;

    uint256 updated;
    updated.SetHex(hash.toStdString());

    // Rows come and go through the insert and remove signals, which a proxy
    // follows on its own; nothing here needs it to filter and sort afresh.
    priv->updateWallet(updated);
}

void MintingTableModel::updateAge()
{
    // If initial load was deferred during IBD, trigger it once IBD completes
    if (!priv->m_loaded && !walletModel->node().isInitialBlockDownload()) {
        beginResetModel();
        priv->refreshWallet();
        endResetModel();
    }

    if (priv->size() == 0) return;

    // Cache difficulty once per update cycle instead of per-row in getDayToMint(),
    // eliminating 2N LOCK(cs_main) acquisitions per tick.
    m_cached_difficulty = walletModel->node().getDifficulty();

    // Force probability recalculation with fresh difficulty
    priv->invalidateCaches();

    Q_EMIT dataChanged(index(0, Age), index(priv->size()-1, MintProbability));
}

int MintingTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int MintingTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QVariant MintingTableModel::data(const QModelIndex &index, int role) const
{
    const Consensus::Params& params = Params().GetConsensus();
    if(!index.isValid())
        return QVariant();
    KernelRecord *rec = static_cast<KernelRecord*>(index.internalPointer());

    switch(role)
    {
      case Qt::DisplayRole:
        switch(index.column())
        {
        case Address:
            return formatTxAddress(rec, false);
        case TxHash:
            return formatTxHash(rec);
        case Age:
            return formatTxAge(rec);
        case Balance:
            return formatTxBalance(rec);
        case CoinDay:
            return formatTxCoinDay(rec);
        case MintProbability:
            return formatDayToMint(rec);
        }
        break;
      case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
        break;
      case Qt::ToolTipRole:
        switch(index.column())
        {
        case MintProbability:
            int interval = this->mintingInterval / 60;
            QString unit = tr("minutes");

            int hours = interval / 60;
            int days = hours  / 24;

            if(hours > 1) {
                interval = hours;
                unit = tr("hours");
            }
            if(days > 1) {
                interval = days;
                unit = tr("days");
            }

            QString str = QString(tr("You have %1 chance to find a POS block if you stake %2 %3 at current difficulty."));
            return str.arg(index.data().toString().toUtf8().constData()).arg(interval).arg(unit);
        }
        break;
      case Qt::EditRole:
        switch(index.column())
        {
        case Address:
            return formatTxAddress(rec, false);
        case TxHash:
            return formatTxHash(rec);
        case Age:
            return qint64(rec->getAge());
        case CoinDay:
            return qint64(rec->getCoinAge());
        case Balance:
            return qint64(rec->nValue);
        case MintProbability:
            return getDayToMint(rec);
        }
        break;
      case Qt::BackgroundRole:
        {
            int minAge = params.nStakeMinAge / 60 / 60;  // minAge in hours
            int maxAge = params.nStakeMaxAge / 60 / 60;  // maxAge in hours
            if(rec->getAge() < minAge)
            {
                return COLOR_MINT_YOUNG;
            }
            else if (rec->getAge() >= minAge && rec->getAge() < maxAge)
            {
                return COLOR_MINT_MATURE;
            }
            else
            {
                return COLOR_MINT_OLD;
            }
        }
        break;
      case Qt::ForegroundRole:
        {
            return COLOR_BLACK;
        }
        break;
    }
    return QVariant();
}

void MintingTableModel::setMintingInterval(int interval)
{
    mintingInterval = interval;
}

QString MintingTableModel::lookupAddress(const std::string &address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if(!label.isEmpty())
    {
        description += label + QString(" ");
    }
    if(label.isEmpty() || tooltip)
    {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

double MintingTableModel::getDayToMint(KernelRecord *wtx) const
{
    int nIntervalMins = mintingInterval / 60;
    double prob = wtx->getProbToMintWithinNMinutes(m_cached_difficulty, nIntervalMins);
    prob = prob * 100;
    return prob;
}

QString MintingTableModel::formatDayToMint(KernelRecord *wtx) const
{
    double prob = getDayToMint(wtx);
    return QString::number(prob, 'f', 6) + "%";
}

QString MintingTableModel::formatTxAddress(const KernelRecord *wtx, bool tooltip) const
{
    return QString::fromStdString(wtx->address);
}

QString MintingTableModel::formatTxHash(const KernelRecord *wtx) const
{
    return QString::fromStdString(wtx->hash.ToString());
}

QString MintingTableModel::formatTxCoinDay(const KernelRecord *wtx) const
{
    return QString::number(wtx->getCoinAge());
}

QString MintingTableModel::formatTxAge(const KernelRecord *wtx) const
{
    int64_t nAge = wtx->getAge();
    QString txtAge;
    if (nAge < 24)
    {
	txtAge = tr("%n hour(s)", "", nAge);
    }
    else
    {
	txtAge = tr("%n day(s)", "", nAge/24);
    }
    return txtAge;
}

QString MintingTableModel::formatTxBalance(const KernelRecord *wtx) const
{
    return BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->nValue);
}

QVariant MintingTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return columns[section];
        }
        else if (role == Qt::TextAlignmentRole)
        {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole)
        {
            switch(section)
            {
            case Address:
                return tr("Destination address of the output.");
            case TxHash:
                return tr("Original transaction id.");
            case Age:
                return tr("Age of the transaction in days.");
            case Balance:
                return tr("Balance of the output.");
            case CoinDay:
                return tr("Coin age expressed as (Value x Age) of the output.");
            case MintProbability:
                return tr("Chance to stake a block within given time interval.");
            }
        }
    }
    return QVariant();
}

QModelIndex MintingTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    KernelRecord *data = priv->index(row);
    if(data)
    {
        return createIndex(row, column, priv->index(row));
    }
    else
    {
        return QModelIndex();
    }
}

void MintingTableModel::updateDisplayUnit()
{
    // emit dataChanged to update Balance column with the current unit
    Q_EMIT dataChanged(index(0, Balance), index(priv->size()-1, Balance));
}
