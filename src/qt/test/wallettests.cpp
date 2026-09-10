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
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <key_io.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <qt/overviewpage.h>
#include <qt/receivecoinsdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/receiverequestdialog.h>

#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
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
