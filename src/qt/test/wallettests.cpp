// Copyright (c) 2015-2020 The Bitcoin Core developers
// Copyright (c) 2015-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/hdaccountmove.h>
#include <qt/hdaccountselector.h>
#include <qt/hdaccountspage.h>
#include <qt/hdaccounttablemodel.h>
#include <qt/mintingfilterproxy.h>
#include <qt/mintingtablemodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletcontroller.h>
#include <qt/walletmodel.h>
#include <key_io.h>
#include <policy/policy.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <qt/overviewpage.h>
#include <qt/receivecoinsdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/receiverequestdialog.h>

#include <functional>
#include <memory>
#include <set>
#include <utility>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QMessageBox>
#include <QDialogButtonBox>

namespace
{
//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, bool cancel = false)
{
    QTimer::singleShot(0, [text, cancel]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(cancel ? QMessageBox::Cancel : QMessageBox::Yes);
                button->setEnabled(true);
                button->click();
            }
        }
    });
}

//! Send coins to address and return txid.
uint256 SendCoins(CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount, bool rbf)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
    sendCoinsDialog.findChild<QFrame*>("frameFee")
        ->findChild<QFrame*>("frameFeeSelection")
        ->findChild<QCheckBox*>("optInRBF")
        ->setCheckState(rbf ? Qt::Checked : Qt::Unchecked);
    uint256 txid;
    boost::signals2::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend();
    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "sendButtonClicked", Q_ARG(bool, false));
    assert(invoked);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const uint256& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Invoke bumpfee on txid and check results.
void BumpFee(TransactionView& view, const uint256& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel);
    } else {
        ConfirmMessage(&text);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     src/qt/test/test_bitcoin-qt  # Linux
//     QT_QPA_PLATFORM=windows src/qt/test/test_bitcoin-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   src/qt/test/test_bitcoin-qt  # macOS
void TestGUI(interfaces::Node& node)
{
    // Set up wallet and chain. TestChain100Setup builds a height-100 chain
    // (89 PoW blocks then 11 PoS blocks), all rewards paying to coinbaseKey.
    // With nCoinbaseMaturity=60 the early PoW coinbases are already mature and
    // spendable, so no extra blocks are needed. (Upstream mined 5 more PoW
    // blocks here for maturity, but Reddcoin rejects PoW past nLastPowHeight=89
    // with "pow-ended".)
    TestChain100Setup test;
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    wallet->LoadWallet();
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type), "", "receive");
        spk_man->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
        wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    }
    {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0 /* block height */, {} /* max height */, reserver, true /* fUpdate */);
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
        QCOMPARE(result.last_scanned_block, node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
        QVERIFY(result.last_failed_block.IsNull());
    }
    wallet->SetBroadcastTransactions(true);

    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    SendCoinsDialog sendCoinsDialog(platformStyle.get());
    TransactionView transactionView(platformStyle.get());
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, std::nullopt);
    sendCoinsDialog.setModel(&walletModel);
    transactionView.setModel(&walletModel);

    {
        // Check balance in send dialog
        QLabel* balanceLabel = sendCoinsDialog.findChild<QLabel*>("labelBalance");
        QString balanceText = balanceLabel->text();
        int unit = walletModel.getOptionsModel()->getDisplayUnit();
        CAmount balance = walletModel.wallet().getBalance();
        QString balanceComparison = BitcoinUnits::formatWithUnit(unit, balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
        QCOMPARE(balanceText, balanceComparison);
    }

    // Send two transactions, and verify they are added to transaction list.
    // The number of pre-existing wallet transactions depends on the height-100
    // TestChain100Setup, so assert relative to the initial count rather than a
    // hardcoded total.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    const int initial_tx_rows = transactionTableModel->rowCount({});
    uint256 txid1 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, false /* rbf */);
    uint256 txid2 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 10 * COIN, true /* rbf */);
    QCOMPARE(transactionTableModel->rowCount({}), initial_tx_rows + 2);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    // Call bumpfee. Test disabled, canceled, enabled, then failing cases.
    BumpFee(transactionView, txid1, true /* expect disabled */, "not BIP 125 replaceable" /* expected error */, false /* cancel */);
    BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, true /* cancel */);
    BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, false /* cancel */);
    BumpFee(transactionView, txid2, true /* expect disabled */, "already bumped" /* expected error */, false /* cancel */);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    QLabel* balanceLabel = overviewPage.findChild<QLabel*>("labelBalance");
    QString balanceText = balanceLabel->text().trimmed();
    int unit = walletModel.getOptionsModel()->getDisplayUnit();
    CAmount balance = walletModel.wallet().getBalance();
    QString balanceComparison = BitcoinUnits::formatWithUnit(unit, balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balanceText, balanceComparison);

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    BitcoinAmountField* amountInput = receiveCoinsDialog.findChild<BitcoinAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    QString address;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Payment information"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("reddcoin:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));
            QVERIFY(address.isEmpty());
            address = receiveRequestDialog->QObject::findChild<QLabel*>("address_content")->text();
            QVERIFY(!address.isEmpty());

            QCOMPARE(uri.count("amount=0.00000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString::fromStdString("0.00000001 " + CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check addition to wallet
    std::vector<std::string> requests = walletModel.wallet().getAddressReceiveRequests();
    QCOMPARE(requests.size(), size_t{1});
    RecentRequestEntry entry;
    CDataStream{MakeUCharSpan(requests[0]), SER_DISK, CLIENT_VERSION} >> entry;
    QCOMPARE(entry.nVersion, int{1});
    QCOMPARE(entry.id, int64_t{1});
    QVERIFY(entry.date.isValid());
    QCOMPARE(entry.recipient.address, address);
    QCOMPARE(entry.recipient.label, QString{"TEST_LABEL_1"});
    QCOMPARE(entry.recipient.amount, CAmount{1});
    QCOMPARE(entry.recipient.message, QString{"TEST_MESSAGE_1"});
    QCOMPARE(entry.recipient.sPaymentRequest, std::string{});
    QCOMPARE(entry.recipient.authenticatedMerchant, QString{});

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);

    // Check removal from wallet
    QCOMPARE(walletModel.wallet().getAddressReceiveRequests().size(), size_t{0});
}

//! Load the height-100 chain's coinbases into a fresh wallet, as TestGUI does.
std::shared_ptr<CWallet> LoadCoinbaseWallet(interfaces::Node& node, TestChain100Setup& test)
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    wallet->LoadWallet();
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type), "", "receive");
        spk_man->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
        wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    }
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0 /* block height */, {} /* max height */, reserver, true /* fUpdate */);
    if (result.status != CWallet::ScanResult::SUCCESS) return nullptr;
    return wallet;
}

//! The per-block refresh visits exactly the rows whose status can still move
//! with a block, a transaction notification re-evaluates the rows it names,
//! and the history filter still finds rows by txid and label.
void TestTransactionTableRefresh(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = LoadCoinbaseWallet(node, test);
    QVERIFY(wallet);

    // A view attaches a dynamic filter proxy, as the history page does, so
    // every row is evaluated and settled the way it is in the client.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    TransactionView transactionView(platformStyle.get());
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, std::nullopt);
    transactionView.setModel(&walletModel);

    TransactionTableModel* model = walletModel.getTransactionTableModel();
    const int rows = model->rowCount({});
    QVERIFY(rows > 0);

    // Record every row range the model reports as changed.
    std::vector<std::pair<int, int>> ranges;
    QMetaObject::Connection recorder = QObject::connect(model, &QAbstractItemModel::dataChanged,
        [&ranges](const QModelIndex& top_left, const QModelIndex& bottom_right) {
            ranges.emplace_back(top_left.row(), bottom_right.row());
        });
    auto covered = [&ranges](int row) {
        for (const auto& range : ranges) {
            if (row >= range.first && row <= range.second) return true;
        }
        return false;
    };
    auto role = [model](int row, int which) {
        return model->data(model->index(row, 0, {}), which);
    };
    auto live = [&role](int row) {
        TransactionStatus status;
        status.status = static_cast<TransactionStatus::Status>(role(row, TransactionTableModel::StatusRole).toInt());
        return status.needsBlockRefresh();
    };

    // The first sweep after loading may visit every row, since a row that
    // has never been refreshed carries the default status. The second sees
    // settled statuses and is the one the client runs on every block.
    model->updateConfirmations();
    ranges.clear();
    model->updateConfirmations();

    // At height 100 with a maturity of 60 the early coinbases are confirmed
    // and the later ones immature, so both kinds are present.
    int live_rows = 0;
    int settled_rows = 0;
    for (int row = 0; row < rows; ++row) {
        if (live(row)) {
            ++live_rows;
            QVERIFY2(covered(row), "a row still waiting on depth was not refreshed");
        } else {
            ++settled_rows;
            QVERIFY2(!covered(row), "a settled row was refreshed");
        }
    }
    QVERIFY(live_rows > 0);
    QVERIFY(settled_rows > 0);

    // A transaction notification re-evaluates the rows it names, whether or
    // not the per-block sweep would have visited them.
    int settled_row = -1;
    for (int row = 0; row < rows && settled_row < 0; ++row) {
        if (!live(row)) settled_row = row;
    }
    QVERIFY(settled_row >= 0);
    const QString settled_hash = role(settled_row, TransactionTableModel::TxHashRole).toString();
    uint256 settled_txid;
    settled_txid.SetHex(settled_hash.toStdString());
    ranges.clear();
    wallet->NotifyTransactionChanged(settled_txid, CT_UPDATED);
    QCoreApplication::processEvents(); // the notification reaches the model through the event loop
    QVERIFY2(covered(settled_row), "a CT_UPDATED notification did not re-evaluate its row");
    QObject::disconnect(recorder);

    // The filter reads the address, label and txid strings only when a
    // search is set; both paths must still answer correctly.
    TransactionFilterProxy proxy;
    proxy.setSourceModel(model);
    QCOMPARE(proxy.rowCount({}), rows);
    proxy.setSearchString(settled_hash.left(16));
    QCOMPARE(proxy.rowCount({}), 1);
    proxy.setSearchString("no such transaction");
    QCOMPARE(proxy.rowCount({}), 0);

    // Labelling the coinbase address must make every row paying to it match
    // a search for that label. Stake rows carry no address and do not count.
    const QString coinbase_address = QString::fromStdString(EncodeDestination(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type)));
    int coinbase_rows = 0;
    for (int row = 0; row < rows; ++row) {
        if (role(row, TransactionTableModel::AddressRole).toString() == coinbase_address) ++coinbase_rows;
    }
    QVERIFY(coinbase_rows > 0);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type), "mined here", "receive");
    }
    QCoreApplication::processEvents(); // the address book change reaches the label index through the event loop
    proxy.setSearchString("mined here");
    QCOMPARE(proxy.rowCount({}), coinbase_rows);
    proxy.setSearchString("");
    QCOMPARE(proxy.rowCount({}), rows);
}

//! Labels are served from the address table model's own index: complete for
//! every address type, current with the address book, and tolerant of
//! another spelling of the same address.
void TestLabelIndex(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    wallet->SetupLegacyScriptPubKeyMan();
    wallet->LoadWallet();

    // Two spellings of one key: a pay-to-pubkey-hash address and a bech32 one.
    CKey key;
    key.MakeNewKey(true);
    const CTxDestination pkhash_dest = GetDestinationForKey(key.GetPubKey(), OutputType::LEGACY);
    const CTxDestination bech32_dest = GetDestinationForKey(key.GetPubKey(), OutputType::BECH32);
    const QString pkhash_address = QString::fromStdString(EncodeDestination(pkhash_dest));
    const QString bech32_address = QString::fromStdString(EncodeDestination(bech32_dest));
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(pkhash_dest, "legacy label", "send");
        wallet->SetAddressBook(bech32_dest, "witness label", "send");
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, std::nullopt);

    AddressTableModel* addresses = walletModel.getAddressTableModel();
    QCOMPARE(addresses->labelForAddress(pkhash_address), QString("legacy label"));
    QCOMPARE(addresses->labelForAddress(bech32_address), QString("witness label"));
    // bech32 is case-insensitive, so the upper-case spelling names the same entry.
    QCOMPARE(addresses->labelForAddress(bech32_address.toUpper()), QString("witness label"));

    // An address that is not in the book, and a string that is not an address.
    CKey other;
    other.MakeNewKey(true);
    QCOMPARE(addresses->labelForAddress(QString::fromStdString(EncodeDestination(GetDestinationForKey(other.GetPubKey(), OutputType::LEGACY)))), QString());
    QCOMPARE(addresses->labelForAddress(QString("not an address")), QString());

    // The index follows the address book through its notifications.
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(pkhash_dest, "renamed", "send");
        wallet->DelAddressBook(bech32_dest);
    }
    QCoreApplication::processEvents();
    QCOMPARE(addresses->labelForAddress(pkhash_address), QString("renamed"));
    QCOMPARE(addresses->labelForAddress(bech32_address), QString());

    // The sign-message dialog rebuilds the wallet's address table with
    // pay-to-pubkey-hash entries only. The table shrinks; the labels of every
    // other address type must survive, since the history view still asks for them.
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(bech32_dest, "witness label", "send");
    }
    QCoreApplication::processEvents();
    walletModel.refresh(/* pk_hash_only */ true);
    addresses = walletModel.getAddressTableModel();
    QCOMPARE(addresses->rowCount({}), 1);
    QCOMPARE(addresses->labelForAddress(pkhash_address), QString("renamed"));
    QCOMPARE(addresses->labelForAddress(bech32_address), QString("witness label"));
}

//! A minting table row, as far as the model exposes it: transaction and amount.
using MintingRows = std::multiset<std::pair<uint256, CAmount>>;

//! The rows a minting table holds.
MintingRows MintingTableRows(const QAbstractItemModel& model)
{
    MintingRows rows;
    for (int row = 0; row < model.rowCount({}); ++row) {
        const QString hash = model.data(model.index(row, MintingTableModel::TxHash, {}), Qt::DisplayRole).toString();
        const CAmount amount = model.data(model.index(row, MintingTableModel::Balance, {}), Qt::EditRole).toLongLong();
        rows.emplace(uint256S(hash.toStdString()), amount);
    }
    return rows;
}

//! The rows it should hold, worked out from the wallet itself: every unspent
//! output of the wallet in a confirmed transaction, a coinbase needing two
//! confirmations.
MintingRows ConfirmedUnspentOutputs(const CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    MintingRows rows;
    for (const auto& entry : wallet.mapWallet) {
        const CWalletTx& wtx = entry.second;
        if (wtx.GetDepthInMainChain() < (wtx.IsCoinBase() ? 2 : 1)) continue;
        for (unsigned int n = 0; n < wtx.tx->vout.size(); ++n) {
            if (!wallet.IsMine(wtx.tx->vout[n]) || wallet.IsSpent(entry.first, n)) continue;
            rows.emplace(entry.first, wtx.tx->vout[n].nValue);
        }
    }
    return rows;
}

//! The minting table lists the wallet's confirmed unspent outputs. A staked
//! block changes it by the rows involved, the kernel leaving and the stake
//! outputs arriving, without the proxy on top being told to start over.
void TestMintingTable(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = LoadCoinbaseWallet(node, test);
    QVERIFY(wallet);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, std::nullopt);

    // A sorting proxy sits on the model, as on the minting page.
    MintingTableModel* model = walletModel.getMintingTableModel();
    MintingFilterProxy proxy;
    proxy.setSourceModel(model);
    proxy.setDynamicSortFilter(true);
    proxy.setSortRole(Qt::EditRole);
    proxy.sort(MintingTableModel::CoinDay, Qt::DescendingOrder);

    // The chain already holds stake blocks, so some of the wallet's outputs
    // are spent kernels: the initial load must leave those out.
    const MintingRows loaded = MintingTableRows(*model);
    QVERIFY(!loaded.empty());
    QVERIFY(loaded == ConfirmedUnspentOutputs(*wallet));
    QCOMPARE(proxy.rowCount({}), model->rowCount({}));

    int inserted = 0;
    int removed = 0;
    int relaid = 0;
    QObject::connect(model, &QAbstractItemModel::rowsInserted, [&inserted](const QModelIndex&, int first, int last) { inserted += last - first + 1; });
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, [&removed](const QModelIndex&, int first, int last) { removed += last - first + 1; });
    QObject::connect(model, &QAbstractItemModel::modelReset, [&relaid] { ++relaid; });
    QObject::connect(&proxy, &QAbstractItemModel::layoutChanged, [&relaid] { ++relaid; });
    QObject::connect(&proxy, &QAbstractItemModel::modelReset, [&relaid] { ++relaid; });

    // A notification for a transaction whose outputs have not changed moves nothing.
    wallet->NotifyTransactionChanged(loaded.begin()->first, CT_UPDATED);
    QCoreApplication::processEvents(); // the notification reaches the model through the event loop
    QCOMPARE(inserted, 0);
    QCOMPARE(removed, 0);
    QCOMPARE(relaid, 0);

    // Stake a block and let the wallet see it. The wallet announces the
    // coinstake under its own hash only; the kernel it spent is not named.
    test.stakeBlocks(1);
    const CTransactionRef coinstake = test.m_coinbase_txns.back();
    QVERIFY(coinstake->IsCoinStake());
    auto rows_of = [](const MintingRows& rows, const uint256& txid) {
        int count = 0;
        for (const auto& row : rows) {
            if (row.first == txid) ++count;
        }
        return count;
    };
    for (const CTxIn& txin : coinstake->vin) {
        QVERIFY2(rows_of(loaded, txin.prevout.hash) > 0, "the kernel was not listed before it was staked");
    }
    QCOMPARE(rows_of(loaded, coinstake->GetHash()), 0);
    {
        LOCK2(wallet->cs_wallet, ::cs_main);
        const CBlockIndex* tip = node.context()->chainman->ActiveChain().Tip();
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    {
        const uint256 tip_hash = WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHash());
        const int tip_height = WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHeight());
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet->ScanForWalletTransactions(tip_hash, tip_height, {} /* max height */, reserver, true /* fUpdate */);
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    QCoreApplication::processEvents();

    const MintingRows staked = MintingTableRows(*model);
    QVERIFY(staked == ConfirmedUnspentOutputs(*wallet));
    QVERIFY2(rows_of(staked, coinstake->GetHash()) > 0, "the stake outputs were not listed");
    QCOMPARE(rows_of(staked, coinstake->GetHash()), inserted);
    QCOMPARE(removed, int(coinstake->vin.size()));
    QCOMPARE(relaid, 0);
    QCOMPARE(proxy.rowCount({}), model->rowCount({}));
}

//! A wallet whose seed does not restore all of its keys shows the notice on the
//! Overview page, with the actions it calls for, until the owner hides it; an
//! ordinary wallet does not. Send all fills the send page with the balance.
void TestKeysNotice(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = LoadCoinbaseWallet(node, test);
    QVERIFY(wallet);

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, std::nullopt);

    {
        // Nothing to say about an ordinary wallet.
        OverviewPage overviewPage(platformStyle.get());
        overviewPage.setWalletModel(&walletModel);
        QVERIFY(!walletModel.wallet().hasKeysSeedDoesNotRestore());
        QVERIFY(overviewPage.findChild<QFrame*>("frameKeysNotice")->isHidden());
    }

    // A plain HD chain (no mnemonic, a version 2 record), and the key that holds
    // the funds derived from its seed on a BIP44 path, as by 4.22.7 to 4.22.9.3.
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        CKey seed;
        seed.MakeNewKey(true);
        CHDChain chain;
        chain.nVersion = CHDChain::VERSION_HD_CHAIN_SPLIT;
        chain.seed_id = seed.GetPubKey().GetID();
        spk_man->LoadHDChain(chain);
        CKeyMetadata meta(GetTime());
        meta.hd_seed_id = chain.seed_id;
        meta.has_key_origin = true;
        meta.key_origin.path = {44 | 0x80000000, 4 | 0x80000000, 0x80000000, 0, 0};
        spk_man->LoadKeyMetadata(test.coinbaseKey.GetPubKey().GetID(), meta);
        spk_man->DecideMixedKeyPaths();
    }
    QVERIFY(walletModel.wallet().hasKeysSeedDoesNotRestore());

    {
        OverviewPage overviewPage(platformStyle.get());
        overviewPage.setWalletModel(&walletModel);
        QFrame* frame = overviewPage.findChild<QFrame*>("frameKeysNotice");
        QVERIFY(!frame->isHidden());
        // With funds: both actions; the full notice in the tooltip.
        QVERIFY(!overviewPage.findChild<QPushButton*>("buttonMoveFunds")->isHidden());
        QVERIFY(!overviewPage.findChild<QPushButton*>("buttonBackupWallet")->isHidden());
        QVERIFY(overviewPage.findChild<QLabel*>("labelKeysNotice")->toolTip().contains("BIP44 paths"));

        // Each action reaches the page's owner.
        int backups{0}, moves{0};
        QObject::connect(&overviewPage, &OverviewPage::backupWalletClicked, [&] { ++backups; });
        QObject::connect(&overviewPage, &OverviewPage::moveFundsClicked, [&] { ++moves; });
        overviewPage.findChild<QPushButton*>("buttonBackupWallet")->click();
        overviewPage.findChild<QPushButton*>("buttonMoveFunds")->click();
        QCOMPARE(backups, 1);
        QCOMPARE(moves, 1);

        // Hidden until the wallet is next loaded: a change of balance does not bring it back.
        overviewPage.findChild<QPushButton*>("buttonHideKeysNotice")->click();
        QVERIFY(frame->isHidden());
        overviewPage.setBalance(walletModel.wallet().getBalances());
        QVERIFY(frame->isHidden());
    }

    {
        // With no funds left: still shown, without the move.
        OverviewPage overviewPage(platformStyle.get());
        overviewPage.setWalletModel(&walletModel);
        overviewPage.setBalance(interfaces::WalletBalances{});
        QVERIFY(!overviewPage.findChild<QFrame*>("frameKeysNotice")->isHidden());
        QVERIFY(overviewPage.findChild<QPushButton*>("buttonMoveFunds")->isHidden());
        QVERIFY(!overviewPage.findChild<QPushButton*>("buttonBackupWallet")->isHidden());
    }

    // Send all: one entry, to the address, with the whole available balance and the fee taken from it.
    SendCoinsDialog sendCoinsDialog(platformStyle.get());
    sendCoinsDialog.setModel(&walletModel);
    sendCoinsDialog.setAddress(QString::fromStdString(EncodeDestination(PKHash())));
    const QString to = QString::fromStdString(EncodeDestination(PKHash(test.coinbaseKey.GetPubKey())));
    sendCoinsDialog.sendAllTo(to, "To wallet new");
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    QCOMPARE(entries->count(), 1);
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    QCOMPARE(entry->findChild<QValidatedLineEdit*>("payTo")->text(), to);
    QCOMPARE(entry->findChild<QLineEdit*>("addAsLabel")->text(), QString("To wallet new"));
    QCOMPARE(entry->findChild<BitcoinAmountField*>("payAmount")->value(), walletModel.wallet().getAvailableBalance(CCoinControl()));
    QVERIFY(entry->findChild<QCheckBox*>("checkboxSubtractFeeFromAmount")->isChecked());
}

//! A wallet that holds HD accounts: the overview gives account 0's Available,
//! what the other accounts hold on a line of its own, and the whole wallet in
//! Total; a change of that amount alone is a change of balance. The messages
//! of the send page and of Move funds say what the other accounts hold.
void TestHDAccounts(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    test.stakeBlocks(1);
    // A bip39 wallet on a mock database, as the other tests here make theirs:
    // CreateWallet would tell wallet controllers of earlier tests about it.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    wallet->LoadWallet();
    LegacyScriptPubKeyMan& spk_man = *wallet->GetOrCreateLegacyScriptPubKeyMan();
    {
        LOCK(wallet->cs_wallet);
        wallet->SetMinVersion(FEATURE_LATEST);
        WalletOptions walletoptions;
        walletoptions.walletType = walletType::bip39Wallet;
        QVERIFY(spk_man.SetupGeneration(walletoptions));
        QVERIFY(spk_man.SetupHDAccounts());
        wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    }
    CTxDestination account_zero;
    std::string dest_error;
    QVERIFY(wallet->GetNewDestination(OutputType::LEGACY, "", account_zero, dest_error));
    CPubKey account_one;
    QVERIFY(spk_man.GetHDAccountPubKey(1, false, 0, account_one));

    // Each paid a coin of the fixture's wallet, in a staked block.
    std::vector<CMutableTransaction> txs;
    {
        LOCK(test.m_wallet->cs_wallet);
        std::vector<COutput> coins;
        test.m_wallet->AvailableCoins(coins);
        auto coin = coins.begin();
        for (const CScript& script : {GetScriptForDestination(account_zero), GetScriptForDestination(PKHash(account_one))}) {
            while (coin != coins.end() && (!coin->fSpendable || coin->nDepth < 20)) ++coin;
            QVERIFY(coin != coins.end());
            test.m_wallet->LockCoin(COutPoint(coin->tx->GetHash(), coin->i));
            CMutableTransaction mtx;
            const CTxOut& prev = coin->tx->tx->vout[coin->i];
            mtx.vout.push_back({prev.nValue - DEFAULT_TRANSACTION_MAXFEE, script});
            mtx.vin.push_back({CTxIn{coin->tx->GetHash(), static_cast<uint32_t>(coin->i)}});
            FillableSigningProvider keystore;
            keystore.AddKey(test.coinbaseKey);
            std::map<COutPoint, Coin> spent;
            spent[mtx.vin[0].prevout].out = prev;
            std::map<int, std::string> input_errors;
            QVERIFY(SignTransaction(mtx, &keystore, spent, SIGHASH_ALL, input_errors));
            txs.push_back(mtx);
            ++coin;
        }
    }
    SetMockTime(GetTime() + 60);
    test.CreateAndProcessPoSBlock(txs, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()), test.m_wallet);
    {
        WalletRescanReserver reserver(*wallet);
        QVERIFY(reserver.reserve());
        QVERIFY(wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, /* fUpdate */ true).status == CWallet::ScanResult::SUCCESS);
        LOCK2(wallet->cs_wallet, ::cs_main);
        wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    }
    const CAmount zero_amount{txs[0].vout[0].nValue};
    const CAmount one_amount{txs[1].vout[0].nValue};

    {
        std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
        OptionsModel optionsModel;
        ClientModel clientModel(node, &optionsModel);
        WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
        const int unit{optionsModel.getDisplayUnit()};

        const interfaces::WalletBalances balances{walletModel.wallet().getBalances()};
        QCOMPARE(balances.balance, zero_amount);
        QCOMPARE(balances.other_accounts, one_amount);
        interfaces::WalletBalances changed{balances};
        changed.other_accounts += 1;
        QVERIFY(changed.balanceChanged(balances));
        QVERIFY(!balances.balanceChanged(balances));

        OverviewPage overviewPage(platformStyle.get());
        overviewPage.setWalletModel(&walletModel);
        overviewPage.setBalance(balances);
        const auto format = [&](CAmount amount) { return BitcoinUnits::formatWithPrivacy(unit, amount, BitcoinUnits::SeparatorStyle::ALWAYS, false); };
        QCOMPARE(overviewPage.findChild<QLabel*>("labelBalance")->text(), format(zero_amount));
        QLabel* other = overviewPage.findChild<QLabel*>("labelOtherAccounts");
        QVERIFY(other && !other->isHidden());
        QCOMPARE(other->text(), format(one_amount));
        QVERIFY(!overviewPage.findChild<QLabel*>("labelOtherAccountsText")->isHidden());
        QCOMPARE(overviewPage.findChild<QLabel*>("labelTotal")->text(), format(zero_amount + one_amount));
        // Nothing in the other accounts: no line.
        overviewPage.setBalance(interfaces::WalletBalances{});
        QVERIFY(other->isHidden());

        // The notice: none for a consistent wallet that owes no scan; one for a restore scan owed.
        QLabel* notice = overviewPage.findChild<QLabel*>("labelHDAccountsNotice");
        QVERIFY(notice && notice->isHidden());
        QVERIFY(WalletBatch(wallet->GetDatabase()).WriteRestoreScan(0));
        overviewPage.setBalance(balances);
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("has not completed"));
        QVERIFY(WalletBatch(wallet->GetDatabase()).EraseRestoreScan());

        // The send page's balance is account 0's.
        SendCoinsDialog sendCoinsDialog(platformStyle.get());
        sendCoinsDialog.setModel(&walletModel);
        QCOMPARE(sendCoinsDialog.findChild<QLabel*>("labelBalance")->text(), BitcoinUnits::formatWithUnit(unit, zero_amount));

        // What the messages of the send page and of Move funds add.
        QVERIFY(GUIUtil::OtherHDAccountsNote(unit, one_amount, false).contains("in other HD accounts. Choose the account to spend from at the top of the send page."));
        QVERIFY(GUIUtil::OtherHDAccountsNote(unit, one_amount, false).contains(BitcoinUnits::formatWithUnit(unit, one_amount)));
        QVERIFY(GUIUtil::OtherHDAccountsNote(unit, one_amount, true).startsWith("This moves account 0 only."));
        QVERIFY(GUIUtil::OtherHDAccountsNote(unit, 0, false).isEmpty());
    }
}

//! Answer the input dialog that opens next with `text` (a number for an
//! integer input), then run `then`, for a dialog that follows it.
void AnswerInput(const QString& text, std::function<void()> then = {})
{
    QTimer::singleShot(0, [text, then]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QInputDialog* dialog = qobject_cast<QInputDialog*>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            if (dialog->inputMode() == QInputDialog::IntInput) {
                dialog->setIntValue(text.toInt());
            } else {
                dialog->setTextValue(text);
            }
            dialog->accept();
        }
        if (then) QTimer::singleShot(0, then);
    });
}

//! Answer Yes to the question that opens next, keeping its text.
void AnswerQuestion(QString* text = nullptr)
{
    QTimer::singleShot(0, [text]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QMessageBox* box = qobject_cast<QMessageBox*>(widget);
            if (!box || !box->isVisible()) continue;
            if (text) *text = box->text();
            if (QAbstractButton* yes = box->button(QMessageBox::Yes)) yes->click();
        }
    });
}

//! A bip39 wallet on a mock database, at the fixture's tip.
std::shared_ptr<CWallet> MakeBip39Wallet(interfaces::Node& node)
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    wallet->LoadWallet();
    LegacyScriptPubKeyMan& spk_man = *wallet->GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(wallet->cs_wallet, ::cs_main);
    wallet->SetMinVersion(FEATURE_LATEST);
    WalletOptions walletoptions;
    walletoptions.walletType = walletType::bip39Wallet;
    if (!spk_man.SetupGeneration(walletoptions)) return nullptr;
    wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    return wallet;
}

//! Pay each script a coin of the fixture's wallet, in one staked block, and
//! have each wallet scan the chain for it.
void PayInABlock(interfaces::Node& node, TestChain100Setup& test, const std::vector<CScript>& scripts, const std::vector<std::shared_ptr<CWallet>>& wallets)
{
    std::vector<CMutableTransaction> txs;
    {
        LOCK(test.m_wallet->cs_wallet);
        std::vector<COutput> coins;
        test.m_wallet->AvailableCoins(coins);
        auto coin = coins.begin();
        for (const CScript& script : scripts) {
            while (coin != coins.end() && (!coin->fSpendable || coin->nDepth < 20)) ++coin;
            QVERIFY(coin != coins.end());
            test.m_wallet->LockCoin(COutPoint(coin->tx->GetHash(), coin->i));
            CMutableTransaction mtx;
            const CTxOut& prev = coin->tx->tx->vout[coin->i];
            mtx.vout.push_back({prev.nValue - DEFAULT_TRANSACTION_MAXFEE, script});
            mtx.vin.push_back({CTxIn{coin->tx->GetHash(), static_cast<uint32_t>(coin->i)}});
            FillableSigningProvider keystore;
            keystore.AddKey(test.coinbaseKey);
            std::map<COutPoint, Coin> spent;
            spent[mtx.vin[0].prevout].out = prev;
            std::map<int, std::string> input_errors;
            QVERIFY(SignTransaction(mtx, &keystore, spent, SIGHASH_ALL, input_errors));
            txs.push_back(mtx);
            ++coin;
        }
    }
    SetMockTime(GetTime() + 60);
    test.CreateAndProcessPoSBlock(txs, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()), test.m_wallet);
    for (const std::shared_ptr<CWallet>& wallet : wallets) {
        WalletRescanReserver reserver(*wallet);
        QVERIFY(reserver.reserve());
        QVERIFY(wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, /* fUpdate */ true).status == CWallet::ScanResult::SUCCESS);
        LOCK2(wallet->cs_wallet, ::cs_main);
        wallet->SetLastBlockProcessed(node.context()->chainman->ActiveChain().Height(), node.context()->chainman->ActiveChain().Tip()->GetBlockHash());
    }
}

//! The usable GUI of HD accounts: the Accounts page and its actions, the
//! account selectors of the send and receive pages, the Account column and
//! filter of the transaction list, the unlock prompt of the first account of
//! an encrypted wallet, Move funds an account at a time, and the restore report.
void TestHDAccountsGUI(interfaces::Node& node)
{
    TestChain100Setup test;
    node.setContext(&test.m_node);
    test.stakeBlocks(1);

    // A wallet with account 1, "savings", paid as account 0 is.
    std::shared_ptr<CWallet> wallet = MakeBip39Wallet(node);
    QVERIFY(wallet);
    CTxDestination account_zero, account_one;
    {
        std::string dest_error;
        QVERIFY(wallet->GetNewDestination(OutputType::LEGACY, "", account_zero, dest_error));
        LOCK(wallet->cs_wallet);
        uint32_t account{0};
        std::vector<bilingual_str> warnings;
        bilingual_str error;
        QVERIFY(wallet->NewHDAccount("savings", /* force */ true, account, account_one, warnings, error));
        QCOMPARE(account, 1U);
    }
    // An encrypted wallet without accounts, whose account 0 is paid.
    std::shared_ptr<CWallet> encrypted = MakeBip39Wallet(node);
    QVERIFY(encrypted);
    CTxDestination encrypted_zero;
    {
        std::string dest_error;
        QVERIFY(encrypted->GetNewDestination(OutputType::LEGACY, "", encrypted_zero, dest_error));
    }
    PayInABlock(node, test, {GetScriptForDestination(account_zero), GetScriptForDestination(account_one), GetScriptForDestination(encrypted_zero)}, {wallet, encrypted});
    const SecureString passphrase{"accounts"};
    QVERIFY(encrypted->EncryptWallet(passphrase));
    QVERIFY(encrypted->Lock());

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    const int unit{optionsModel.getDisplayUnit()};
    std::vector<interfaces::WalletHDAccount> accounts{walletModel.wallet().listHDAccounts(/* with_balances */ true)};
    QCOMPARE(int(accounts.size()), 2);
    const CAmount zero_amount{accounts[0].trusted};
    const CAmount one_amount{accounts[1].trusted};
    QVERIFY(zero_amount > 0 && one_amount > 0);

    {
        // The Accounts page: account 0 and account 1, with what each holds.
        HDAccountsPage page(platformStyle.get());
        page.setModel(&walletModel);
        HDAccountTableModel* model = page.tableModel();
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->index(0, HDAccountTableModel::Label).data().toString(), QString("Default"));
        QCOMPARE(model->index(1, HDAccountTableModel::Number).data().toUInt(), 1U);
        QCOMPARE(model->index(1, HDAccountTableModel::Label).data().toString(), QString("savings"));
        QCOMPARE(model->index(1, HDAccountTableModel::State).data().toString(), QString("Active"));
        QCOMPARE(model->index(1, HDAccountTableModel::Available).data(Qt::EditRole).toLongLong(), qint64(one_amount));
        QCOMPARE(model->index(0, HDAccountTableModel::Available).data(Qt::EditRole).toLongLong(), qint64(zero_amount));
        QCOMPARE(model->index(1, HDAccountTableModel::Used).data().toString(), QString("Yes"));
        // Account 1 is paid: the next can be created.
        QVERIFY(page.newAccountButton()->isEnabled());
        QVERIFY(page.newAccountRefusalLabel()->isHidden());
        QVERIFY(page.noticeLabel()->isHidden());
        // Nothing selected: nothing to rename or retire. Account 0 has no label or state of its own.
        QVERIFY(!page.renameButton()->isEnabled());
        page.tableView()->selectRow(0);
        QVERIFY(!page.renameButton()->isEnabled());
        QVERIFY(!page.retireButton()->isEnabled());

        // Rename account 1.
        page.tableView()->selectRow(1);
        QVERIFY(page.renameButton()->isEnabled());
        AnswerInput("rainy day");
        page.renameAccount();
        QCOMPARE(walletModel.wallet().listHDAccounts(false)[1].label, std::string{"rainy day"});

        // New account: account 2, "travel". Its first address is handed out
        // and not yet paid; the next account is refused until it is paid.
        QString created;
        QObject::connect(&page, &HDAccountsPage::message, [&](const QString&, const QString& text, unsigned int) { created = text; });
        AnswerInput("travel");
        page.newAccount();
        QVERIFY(created.startsWith("HD account 2 was created."));
        page.refresh();
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(model->index(2, HDAccountTableModel::Label).data().toString(), QString("travel"));
        QVERIFY(!page.newAccountButton()->isEnabled());
        QVERIFY(!page.newAccountRefusalLabel()->isHidden());
        QVERIFY(page.newAccountRefusalLabel()->text().contains("Account 2 has not received a confirmed payment"));

        // Its unpaid address.
        page.tableView()->selectRow(2);
        const std::vector<CTxDestination> unpaid{walletModel.wallet().getUnpaidHDAccountAddresses(2)};
        QCOMPARE(int(unpaid.size()), 1);
        QString text;
        ConfirmMessage(&text);
        page.showUnpaidAddresses();
        QVERIFY(text.contains(QString::fromStdString(EncodeDestination(unpaid[0]))));

        // Retire account 2, then bring it back, and retire it again.
        QCOMPARE(page.retireButton()->text(), QString("Re&tire"));
        AnswerQuestion();
        page.retireAccount();
        QVERIFY(walletModel.wallet().listHDAccounts(false)[2].retired);
        page.refresh();
        page.tableView()->selectRow(2);
        QCOMPARE(page.retireButton()->text(), QString("Bring &back"));
        page.retireAccount();
        QVERIFY(!walletModel.wallet().listHDAccounts(false)[2].retired);
        // The page reads the wallet again when it is in view; this one is not.
        page.refresh();
        page.tableView()->selectRow(2);
        QCOMPARE(page.retireButton()->text(), QString("Re&tire"));
        AnswerQuestion();
        page.retireAccount();
        QVERIFY(walletModel.wallet().listHDAccounts(false)[2].retired);
    }

    {
        // Send: spend from account 1. The balance is account 1's; changing
        // the account clears the coins chosen; use available balance takes
        // account 1's coins. A retired account can still be spent.
        SendCoinsDialog sendCoinsDialog(platformStyle.get());
        sendCoinsDialog.setModel(&walletModel);
        HDAccountSelector* selector = sendCoinsDialog.hdAccountSelector();
        QVERIFY(!selector->isHidden());
        QCOMPARE(selector->comboBox()->count(), 3);
        QCOMPARE(selector->comboBox()->itemText(0), QString("Account 0 (Default)"));
        QVERIFY(selector->comboBox()->itemText(2).contains("retired"));
        QCOMPARE(sendCoinsDialog.findChild<QLabel*>("labelBalance")->text(), BitcoinUnits::formatWithUnit(unit, zero_amount));
        selector->setAccount(1);
        QCOMPARE(selector->account(), 1U);
        QCOMPARE(sendCoinsDialog.findChild<QLabel*>("labelBalance")->text(), BitcoinUnits::formatWithUnit(unit, one_amount));
        QCOMPARE(sendCoinsDialog.coinControl().m_hd_account.value_or(99), 1U);
        // A coin chosen by hand: cleared when the account changes.
        sendCoinsDialog.coinControl().Select(COutPoint(uint256::ONE, 0));
        selector->setAccount(0);
        QVERIFY(!sendCoinsDialog.coinControl().HasSelected());
        QCOMPARE(sendCoinsDialog.coinControl().m_hd_account.value_or(99), 0U);
        selector->setAccount(1);
        QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
        SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
        entry->findChild<QPushButton*>("useAvailableBalanceButton")->click();
        QCOMPARE(entry->findChild<BitcoinAmountField*>("payAmount")->value(), one_amount);
        // Move funds sends account 0.
        sendCoinsDialog.sendAllTo(QString::fromStdString(EncodeDestination(PKHash(test.coinbaseKey.GetPubKey()))), "To wallet new");
        QCOMPARE(selector->account(), 0U);
        entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
        QCOMPARE(entry->findChild<BitcoinAmountField*>("payAmount")->value(), zero_amount);
    }

    {
        // Receive: an address of account 1; the retired account is not offered.
        ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
        receiveCoinsDialog.setModel(&walletModel);
        HDAccountSelector* selector = receiveCoinsDialog.hdAccountSelector();
        QVERIFY(!selector->isHidden());
        QCOMPARE(selector->comboBox()->count(), 2);
        selector->setAccount(1);
        RecentRequestsTableModel* requests = walletModel.getRecentRequestsTableModel();
        const int before{requests->rowCount({})};
        receiveCoinsDialog.findChild<QPushButton*>("receiveButton")->click();
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("ReceiveRequestDialog")) widget->close();
        }
        QCOMPARE(requests->rowCount({}), before + 1);
        QTableView* view = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
        QVERIFY(!view->isColumnHidden(RecentRequestsTableModel::Account));
        bool found{false};
        for (int row = 0; row < requests->rowCount({}); ++row) {
            const CTxDestination dest{DecodeDestination(requests->entry(row).recipient.address.toStdString())};
            if (walletModel.wallet().getHDAccountOfScript(GetScriptForDestination(dest)) != 1) continue;
            QCOMPARE(requests->index(row, RecentRequestsTableModel::Account).data().toUInt(), 1U);
            found = true;
        }
        QVERIFY(found);
    }

    {
        // The transaction list: the Account column, and the filter.
        TransactionView transactionView(platformStyle.get());
        transactionView.setModel(&walletModel);
        QTableView* table = transactionView.findChild<QTableView*>("transactionView");
        QVERIFY(!table->isColumnHidden(TransactionTableModel::Account));
        QCOMPARE(table->model()->rowCount(), 2);
        QComboBox* filter{nullptr};
        for (QComboBox* combo : transactionView.findChildren<QComboBox*>()) {
            if (combo->itemText(0) == "All accounts") filter = combo;
        }
        QVERIFY(filter && !filter->isHidden());
        const int index{filter->findData(1u)};
        QVERIFY(index > 0);
        filter->setCurrentIndex(index);
        transactionView.chooseHDAccount(index);
        QCOMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->model()->index(0, TransactionTableModel::Account).data().toUInt(), 1U);
        transactionView.chooseHDAccount(0);
        QCOMPARE(table->model()->rowCount(), 2);
    }

    {
        // The first account of an encrypted, locked wallet: the passphrase
        // is asked for, the account is made, and the wallet is locked again.
        WalletModel encryptedModel(interfaces::MakeWallet(encrypted), clientModel, platformStyle.get());
        int asked{0};
        QObject::connect(&encryptedModel, &WalletModel::requireUnlock, [&] {
            ++asked;
            QVERIFY(encryptedModel.setWalletLocked(false, passphrase));
        });
        HDAccountsPage page(platformStyle.get());
        page.setModel(&encryptedModel);
        QVERIFY(page.newAccountButton()->isEnabled());
        QString confirm;
        AnswerInput("first", [&confirm] { AnswerQuestion(&confirm); });
        page.newAccount();
        QCOMPARE(asked, 1);
        QVERIFY(confirm.contains("marks this wallet"));
        QCOMPARE(int(encryptedModel.wallet().listHDAccounts(false).size()), 2);
        QCOMPARE(encryptedModel.getEncryptionStatus(), WalletModel::Locked);
    }

    {
        // An account created outside the GUI, as by the RPC console: the
        // receive and transaction pages show the Account column and the
        // account once the wallet tells the GUI. A new address of an account
        // changes nothing listed, and tells nothing.
        std::shared_ptr<CWallet> plain = MakeBip39Wallet(node);
        QVERIFY(plain);
        // As loading a wallet does (AddWallet).
        plain->ConnectScriptPubKeyManNotifiers();
        WalletModel plainModel(interfaces::MakeWallet(plain), clientModel, platformStyle.get());
        ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
        receiveCoinsDialog.setModel(&plainModel);
        TransactionView transactionView(platformStyle.get());
        transactionView.setModel(&plainModel);
        QTableView* requests = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
        QTableView* table = transactionView.findChild<QTableView*>("transactionView");
        QVERIFY(requests->isColumnHidden(RecentRequestsTableModel::Account));
        QVERIFY(table->isColumnHidden(TransactionTableModel::Account));

        int told{0};
        QObject::connect(&plainModel, &WalletModel::hdAccountsChanged, [&told] { ++told; });
        {
            LOCK(plain->cs_wallet);
            uint32_t account{0};
            CTxDestination first;
            std::vector<bilingual_str> warnings;
            bilingual_str error;
            QVERIFY(plain->NewHDAccount("outside", /* force */ true, account, first, warnings, error));
            QCOMPARE(account, 1U);
        }
        QCoreApplication::processEvents();
        QVERIFY(told > 0);
        QVERIFY(!requests->isColumnHidden(RecentRequestsTableModel::Account));
        QVERIFY(!table->isColumnHidden(TransactionTableModel::Account));
        QComboBox* filter{nullptr};
        for (QComboBox* combo : transactionView.findChildren<QComboBox*>()) {
            if (combo->itemText(0) == "All accounts") filter = combo;
        }
        QVERIFY(filter);
        QCOMPARE(filter->itemText(1), QString("Account 0 (Default)"));
        QCOMPARE(filter->itemText(2), QString("Account 1 (outside)"));

        told = 0;
        CTxDestination dest;
        bilingual_str error;
        QVERIFY(plainModel.wallet().getNewHDAccountDestination(1, OutputType::LEGACY, "", dest, error));
        QCoreApplication::processEvents();
        QCOMPARE(told, 0);
        {
            LOCK(plain->cs_wallet);
            QVERIFY(plain->SetHDAccount(1, std::string{"renamed"}, std::nullopt, error));
        }
        QCoreApplication::processEvents();
        QVERIFY(told > 0);
        QCOMPARE(filter->itemText(2), QString("Account 1 (renamed)"));
    }

    {
        // Move funds: account 0 to account 0 of the new wallet, account 1 to
        // its account 1, each in its own transaction; account 2 holds nothing.
        const std::vector<HDAccountMove> plan{PlanHDAccountMove(walletModel.wallet().listHDAccounts(true))};
        QCOMPARE(int(plan.size()), 2);
        QCOMPARE(plan[1].from, 1U);
        QCOMPARE(plan[1].to, 1U);
        QVERIFY(MovesHDAccounts(plan));
        QVERIFY(DescribeHDAccountMove(plan, unit).contains("account 1 (rainy day) to account 1 of the new wallet"));
        // Funds in account 2 alone would go to account 0 of the new wallet.
        std::vector<interfaces::WalletHDAccount> only_two(1);
        only_two[0].number = 2;
        only_two[0].trusted = 5;
        QCOMPARE(PlanHDAccountMove(only_two)[0].to, 0U);
        QVERIFY(MovesHDAccounts(PlanHDAccountMove(only_two)));
        QVERIFY(!MovesHDAccounts(PlanHDAccountMove({accounts[0]})));

        std::shared_ptr<CWallet> target = MakeBip39Wallet(node);
        QVERIFY(target);
        // As a wallet loaded by the node does: its payments go to the mempool.
        wallet->SetBroadcastTransactions(true);
        WalletModel targetModel(interfaces::MakeWallet(target), clientModel, platformStyle.get());
        QString question;
        AnswerQuestion(&question);
        QString result;
        QVERIFY2(MoveHDAccounts(walletModel, targetModel, plan, nullptr, result), result.toStdString().c_str());
        QVERIFY(result.startsWith("Moved:"));
        // Two transactions, each spending one account, to the account of the new wallet planned.
        int moved{0};
        for (const interfaces::WalletTx& wtx : walletModel.wallet().getWalletTxs()) {
            for (size_t i = 0; i < wtx.tx->vout.size(); ++i) {
                const CScript& script = wtx.tx->vout[i].scriptPubKey;
                if (!WITH_LOCK(target->cs_wallet, return target->IsMine(script))) continue;
                QCOMPARE(target->GetHDAccountOfScript(script), wtx.txin_account.at(0));
                ++moved;
            }
        }
        QCOMPARE(moved, 2);
        const std::vector<interfaces::WalletHDAccount> target_accounts{targetModel.wallet().listHDAccounts(false)};
        QCOMPARE(int(target_accounts.size()), 2);
        QCOMPARE(target_accounts[1].label, std::string{"rainy day"});
    }

    // The restore report.
    interfaces::WalletRestoreReport report;
    report.hd_type = "bip44";
    report.first_path = "m/44'/4'/0'/0/0";
    report.used_addresses = 3;
    report.accounts_found = {1, 2};
    report.history = true;
    bool no_history{true};
    QString text{RestoreReportText(report, "restored", no_history)};
    QVERIFY(!no_history);
    QVERIFY(text.contains("restored as a bip44 wallet. Its first address is at m/44'/4'/0'/0/0."));
    QVERIFY(text.contains("HD accounts found: 1, 2."));
    report = {};
    report.hd_type = "bip39";
    text = RestoreReportText(report, "restored", no_history);
    QVERIFY(no_history);
    QVERIFY(text.contains("No HD account above 0 was found."));
    QVERIFY(text.contains("No history was found."));
    // A scan not finished is not a restore without history.
    report.scan_owed = true;
    text = RestoreReportText(report, "restored", no_history);
    QVERIFY(!no_history);
    QVERIFY(text.contains("has not finished"));
}

//! Qt on macOS crashes inside the framework on the "minimal" platform when it
//! looks up unimplemented cocoa functions (https://bugreports.qt.io/browse/QTBUG-49686).
bool SkipOnMacMinimalPlatform(const char* test_name)
{
#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        QWARN(QString("Skipping %1 on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_reddcoin-qt' on mac, or else use a linux or windows build.")
                  .arg(test_name).toUtf8().constData());
        return true;
    }
#endif
    (void)test_name;
    return false;
}

} // namespace

void WalletTests::walletTests()
{
    if (SkipOnMacMinimalPlatform("WalletTests")) return;
    TestGUI(m_node);
}

void WalletTests::transactionTableTests()
{
    if (SkipOnMacMinimalPlatform("transactionTableTests")) return;
    TestTransactionTableRefresh(m_node);
}

void WalletTests::labelIndexTests()
{
    if (SkipOnMacMinimalPlatform("labelIndexTests")) return;
    TestLabelIndex(m_node);
}

void WalletTests::mintingTableTests()
{
    if (SkipOnMacMinimalPlatform("mintingTableTests")) return;
    TestMintingTable(m_node);
}

void WalletTests::keysNoticeTests()
{
    if (SkipOnMacMinimalPlatform("keysNoticeTests")) return;
    TestKeysNotice(m_node);
}

void WalletTests::hdAccountsTests()
{
    if (SkipOnMacMinimalPlatform("hdAccountsTests")) return;
    TestHDAccounts(m_node);
}

void WalletTests::hdAccountsGUITests()
{
    if (SkipOnMacMinimalPlatform("hdAccountsGUITests")) return;
    TestHDAccountsGUI(m_node);
}
