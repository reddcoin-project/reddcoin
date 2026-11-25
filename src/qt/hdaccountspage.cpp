// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/hdaccountspage.h>

#include <key_io.h>
#include <node/ui_interface.h>
#include <qt/guiutil.h>
#include <qt/hdaccounttablemodel.h>
#include <qt/platformstyle.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <optional>

NewHDAccountDialog::NewHDAccountDialog(uint32_t account, std::optional<uint32_t> unpaid, QWidget* parent)
    : QDialog(parent),
      m_label(new QLineEdit(this))
{
    setWindowTitle(tr("New HD account"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* prompt = new QLabel(tr("A label for account %1, and for its first address (optional):").arg(account), this);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);
    layout->addWidget(m_label);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_create = buttons->addButton(tr("&Create"), QDialogButtonBox::AcceptRole);
    if (unpaid) {
        QLabel* rule = new QLabel(tr("Account %1 has not received a confirmed payment. A restore from the phrase, in %2 or in another BIP44 wallet, looks for an account only after the one before it was paid: until account %1 is paid, a restore may not find account %3 and its coins.").arg(*unpaid).arg(PACKAGE_NAME).arg(account), this);
        rule->setObjectName("ruleLabel");
        rule->setWordWrap(true);
        layout->addWidget(rule);
        m_force = new QCheckBox(tr("Create account %1 anyway").arg(account), this);
        layout->addWidget(m_force);
        m_create->setEnabled(false);
        connect(m_force, &QCheckBox::toggled, m_create, &QPushButton::setEnabled);
    }
    m_create->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString NewHDAccountDialog::label() const
{
    return m_label->text();
}

bool NewHDAccountDialog::force() const
{
    return m_force && m_force->isChecked();
}

HDAccountsPage::HDAccountsPage(const PlatformStyle* platform_style, QWidget* parent)
    : QWidget(parent),
      m_platform_style(platform_style),
      m_intro(new QLabel(this)),
      m_notice(new QLabel(this)),
      m_view(new QTableView(this)),
      m_new_button(new QPushButton(tr("&New account…"), this)),
      m_rename_button(new QPushButton(tr("&Rename…"), this)),
      m_retire_button(new QPushButton(tr("Re&tire"), this)),
      m_unpaid_button(new QPushButton(tr("&Unpaid addresses"), this)),
      m_find_button(new QPushButton(tr("&Find accounts…"), this)),
      m_new_refusal(new QLabel(this)),
      m_earlier_seed(new QLabel(this))
{
    m_intro->setWordWrap(true);
    m_intro->setText(tr("HD accounts keep funds apart in one wallet: each account has its own addresses and balance, "
                        "a payment spends one account and returns its change to it, and coins of two accounts are never "
                        "spent together. The phrase or seed of the wallet restores every account. A restore finds "
                        "accounts by number, not by label."));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    m_notice->setStyleSheet(QStringLiteral("QLabel { background-color: #fff3cd; color: #664d03; padding: 4px; }"));
    m_new_refusal->setWordWrap(true);
    m_new_refusal->setVisible(false);
    m_earlier_seed->setWordWrap(true);
    m_earlier_seed->setVisible(false);

    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setAlternatingRowColors(true);
    m_view->verticalHeader()->hide();
    m_view->setTabKeyNavigation(false);

    m_new_button->setToolTip(tr("Create the next account"));
    m_rename_button->setToolTip(tr("Change the label of the selected account"));
    m_retire_button->setToolTip(tr("A retired account gives no new address; its coins are still watched and spendable"));
    m_unpaid_button->setToolTip(tr("The receive addresses of the selected account handed out and not yet paid"));
    m_find_button->setToolTip(tr("Scan the block chain for accounts of this wallet's seed, as a restore does"));
    if (m_platform_style->getImagesOnButtons()) {
        m_new_button->setIcon(m_platform_style->SingleColorIcon(":/icons/add"));
        m_rename_button->setIcon(m_platform_style->SingleColorIcon(":/icons/edit"));
    }

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addWidget(m_new_button);
    buttons->addWidget(m_rename_button);
    buttons->addWidget(m_retire_button);
    buttons->addWidget(m_unpaid_button);
    buttons->addStretch();
    buttons->addWidget(m_find_button);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(m_notice);
    layout->addWidget(m_view, 1);
    layout->addLayout(buttons);
    layout->addWidget(m_new_refusal);
    layout->addWidget(m_earlier_seed);

    connect(m_new_button, &QPushButton::clicked, this, &HDAccountsPage::newAccount);
    connect(m_rename_button, &QPushButton::clicked, this, &HDAccountsPage::renameAccount);
    connect(m_retire_button, &QPushButton::clicked, this, &HDAccountsPage::retireAccount);
    connect(m_unpaid_button, &QPushButton::clicked, this, &HDAccountsPage::showUnpaidAddresses);
    connect(m_find_button, &QPushButton::clicked, this, &HDAccountsPage::findAccounts);
    updateButtons();
}

HDAccountsPage::~HDAccountsPage()
{
    stopScan();
}

void HDAccountsPage::stopScan()
{
    if (!m_scan_thread.joinable()) return;
    if (m_model) m_model->wallet().abortRescan();
    m_scan_thread.join();
    m_scan_unlock.reset();
}

void HDAccountsPage::setModel(WalletModel* model)
{
    // The scan uses the wallet of the model it was started on.
    stopScan();
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    m_view->setModel(nullptr);
    delete m_table_model;
    m_table_model = nullptr;
    if (!m_model) return;

    m_table_model = new HDAccountTableModel(m_model, this);
    m_view->setModel(m_table_model);
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(HDAccountTableModel::Label, QHeaderView::Stretch);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &HDAccountsPage::updateButtons);
    connect(m_table_model, &QAbstractItemModel::modelReset, this, &HDAccountsPage::updateButtons);

    // What the page shows takes a pass over the wallet's transactions: read
    // it while the page is in view, and when it comes into view.
    auto refresh_in_view = [this] { if (isVisible()) refresh(); };
    connect(m_model, &WalletModel::balanceChanged, this, refresh_in_view);
    connect(m_model, &WalletModel::hdAccountsChanged, this, refresh_in_view);
    connect(m_model, &WalletModel::encryptionStatusChanged, this, refresh_in_view);
    // Before the model goes: a scan of its wallet stops first.
    connect(m_model, &WalletModel::unload, this, &HDAccountsPage::stopScan);
    // Its view, this page, goes before it: a scan has stopped by then.
    connect(m_model, &QObject::destroyed, this, [this] { m_model = nullptr; });
    refresh();
}

void HDAccountsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void HDAccountsPage::refresh()
{
    if (!m_model) return;
    m_state = m_model->wallet().getHDAccountsState();
    if (m_table_model) {
        // Keep the selected account selected.
        const interfaces::WalletHDAccount* selected = selectedAccount();
        const std::optional<uint32_t> number{selected ? std::optional<uint32_t>{selected->number} : std::nullopt};
        m_table_model->refresh();
        if (number) {
            for (int row = 0; row < m_table_model->rowCount(); ++row) {
                if (m_table_model->account(row)->number == *number) m_view->selectRow(row);
            }
        }
    }

    QStringList notices;
    if (m_state.inconsistent) {
        notices << tr("This wallet holds keys of HD accounts without the mark that says it holds them, as a salvage with an earlier wallet tool leaves it.");
    }
    if (m_state.scan_owed || (m_state.discovery_pending && m_model->getEncryptionStatus() == WalletModel::Locked)) {
        notices << tr("The scan of this restored wallet is not finished. Unlock the wallet to finish it.");
    } else if (m_state.unlock_owed) {
        notices << tr("Unlock the wallet to finish finding addresses of your HD accounts.");
    } else if (m_state.discovery_pending) {
        notices << tr("The wallet is looking for its HD accounts in the block chain.");
    }
    m_notice->setText(notices.join("\n"));
    m_notice->setVisible(!notices.isEmpty());

    m_earlier_seed->setText(tr("This wallet also holds %n key(s) of a seed it no longer has. Its phrase or seed does not restore them: only a copy of the wallet file does.", "", m_state.earlier_seed_keys));
    m_earlier_seed->setVisible(m_state.earlier_seed_keys > 0);
    updateButtons();
}

uint32_t HDAccountsPage::highestAccount() const
{
    if (!m_table_model || m_table_model->rowCount() == 0) return 0;
    return m_table_model->account(m_table_model->rowCount() - 1)->number;
}

const interfaces::WalletHDAccount* HDAccountsPage::selectedAccount() const
{
    if (!m_table_model || !m_view->selectionModel()) return nullptr;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    return rows.isEmpty() ? nullptr : m_table_model->account(rows.first().row());
}

void HDAccountsPage::updateButtons()
{
    const bool usable{m_model && m_state.supported && !isScanning()};
    const interfaces::WalletHDAccount* selected = selectedAccount();
    const bool above_zero{selected && selected->number > 0};

    const QString refusal{QString::fromStdString(m_state.new_account_refusal.translated)};
    // The account rule alone can be overridden: New account says what that means.
    m_new_button->setEnabled(usable && (refusal.isEmpty() || m_state.new_account_forceable));
    if (refusal.isEmpty()) {
        m_new_refusal->clear();
    } else if (m_state.new_account_forceable) {
        m_new_refusal->setText(tr("Account %1 has not received a confirmed payment: a restore from the phrase may not find an account created before it is paid.").arg(highestAccount()));
    } else {
        m_new_refusal->setText(tr("New account is not available: %1").arg(refusal));
    }
    m_new_refusal->setVisible(usable && !refusal.isEmpty());

    m_rename_button->setEnabled(usable && above_zero);
    m_retire_button->setEnabled(usable && above_zero);
    m_retire_button->setText(above_zero && selected->retired ? tr("Bring &back") : tr("Re&tire"));
    m_unpaid_button->setEnabled(usable && above_zero);
    m_find_button->setEnabled(usable);
    if (m_model && !m_state.supported) {
        m_intro->setText(tr("HD accounts are available in legacy HD wallets with private keys only. Use a separate wallet to keep funds apart."));
    }
}

void HDAccountsPage::newAccount()
{
    if (!m_model) return;
    refresh();
    if (!m_state.new_account_refusal.empty() && !m_state.new_account_forceable) {
        Q_EMIT message(tr("New HD account"), QString::fromStdString(m_state.new_account_refusal.translated), CClientUIInterface::MSG_WARNING);
        return;
    }
    const uint32_t highest{highestAccount()};
    const std::optional<uint32_t> unpaid{m_state.new_account_forceable ? std::optional<uint32_t>{highest} : std::nullopt};
    NewHDAccountDialog dialog(highest + 1, unpaid, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString label{dialog.label()};
    const bool force{dialog.force()};

    if (!m_state.holds_accounts) {
        // The first account marks the wallet, and it keeps its seed from then on.
        QString text{tr("The first HD account marks this wallet: versions of %1 without HD accounts then refuse to open it, as requiring a newer version. The wallet itself is intact.").arg(PACKAGE_NAME)};
        if (!m_model->wallet().isCrypted()) {
            text += "\n\n" + tr("This wallet keeps its seed when it is encrypted: any copy of the wallet file made before that holds the seed unencrypted, and has to be destroyed. Make a new backup.");
            if (!m_model->wallet().bip39Enabled()) {
                text += "\n\n" + tr("Encrypt this wallet before creating accounts if you intend to encrypt it at all.");
            }
        }
        text += "\n\n" + tr("Create the first account?");
        if (QMessageBox::question(this, tr("New HD account"), text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    }

    uint32_t account{0};
    CTxDestination first;
    std::vector<bilingual_str> warnings;
    bilingual_str error;
    std::optional<WalletModel::UnlockContext> unlock;
    bool created = m_model->wallet().newHDAccount(label.toStdString(), force, account, first, warnings, error);
    if (!created && m_model->getEncryptionStatus() == WalletModel::Locked) {
        // The first account, and an account whose keys are not yet derived,
        // need the seed: ask for the passphrase, and try again.
        unlock.emplace(m_model->requestUnlock());
        if (!unlock->isValid()) return;
        warnings.clear();
        error = {};
        created = m_model->wallet().newHDAccount(label.toStdString(), force, account, first, warnings, error);
    }
    if (!created) {
        Q_EMIT message(tr("New HD account"), QString::fromStdString(error.translated), CClientUIInterface::MSG_ERROR);
        refresh();
        return;
    }
    Q_EMIT m_model->hdAccountsChanged();

    QString text{tr("HD account %1 was created. Its first address is %2.").arg(account).arg(QString::fromStdString(EncodeDestination(first)))};
    text += "\n\n" + tr("A restore finds accounts by number, not by label: note the number.");
    if (force) text += "\n\n" + tr("Pay account %1, so that a restore from the phrase finds account %2.").arg(highest).arg(account);
    for (const bilingual_str& warning : warnings) {
        // The account rule, said above in the words of this page.
        if (force && warning.original == m_state.new_account_refusal.original) continue;
        text += "\n\n" + QString::fromStdString(warning.translated);
    }
    Q_EMIT message(tr("New HD account"), text, CClientUIInterface::MSG_INFORMATION);
}

void HDAccountsPage::renameAccount()
{
    const interfaces::WalletHDAccount* selected = selectedAccount();
    if (!m_model || !selected || selected->number == 0) return;
    const uint32_t number{selected->number};
    bool ok{false};
    const QString label = QInputDialog::getText(this, tr("Rename HD account"),
        tr("The new label of account %1:").arg(number), QLineEdit::Normal, QString::fromStdString(selected->label), &ok);
    if (!ok) return;
    bilingual_str error;
    if (!m_model->wallet().setHDAccount(number, label.toStdString(), std::nullopt, error)) {
        Q_EMIT message(tr("Rename HD account"), QString::fromStdString(error.translated), CClientUIInterface::MSG_ERROR);
        return;
    }
    Q_EMIT m_model->hdAccountsChanged();
}

void HDAccountsPage::retireAccount()
{
    const interfaces::WalletHDAccount* selected = selectedAccount();
    if (!m_model || !selected || selected->number == 0) return;
    const uint32_t number{selected->number};
    const bool retire{!selected->retired};
    if (retire && QMessageBox::question(this, tr("Retire HD account"),
            tr("Account %1 will give no new address. Its coins are still watched and spendable, and the account can be brought back.").arg(number) + "\n\n" + tr("Retire account %1?").arg(number),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    bilingual_str error;
    if (!m_model->wallet().setHDAccount(number, std::nullopt, retire, error)) {
        Q_EMIT message(retire ? tr("Retire HD account") : tr("Bring back HD account"), QString::fromStdString(error.translated), CClientUIInterface::MSG_ERROR);
        return;
    }
    Q_EMIT m_model->hdAccountsChanged();
}

void HDAccountsPage::showUnpaidAddresses()
{
    const interfaces::WalletHDAccount* selected = selectedAccount();
    if (!m_model || !selected || selected->number == 0) return;
    const uint32_t number{selected->number};
    QStringList addresses;
    for (const CTxDestination& dest : m_model->wallet().getUnpaidHDAccountAddresses(number)) {
        addresses << QString::fromStdString(EncodeDestination(dest));
    }
    QString text;
    if (addresses.isEmpty()) {
        text = tr("Account %1 has no receive address that is handed out and not yet paid.").arg(number);
    } else {
        text = tr("The receive addresses of account %1 handed out and not yet paid:").arg(number) + "\n\n" + addresses.join("\n");
        text += "\n\n" + tr("A restore finds addresses up to 20 past the last one paid: use these before asking for new ones.");
    }
    QMessageBox box(QMessageBox::Information, tr("Unpaid addresses"), text, QMessageBox::Ok, this);
    box.setDefaultButton(QMessageBox::Ok);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}

void HDAccountsPage::findAccounts()
{
    if (!m_model || isScanning()) return;
    bool ok{false};
    const int count = QInputDialog::getInt(this, tr("Find HD accounts"),
        tr("The whole block chain is scanned for the accounts of this wallet's seed, as a restore does: the account after the highest used one is looked at, and the account after each one found used.\n\n"
           "An account created ahead of the account before it being paid is found only if this many accounts past the highest used one are looked at:"),
        1, 1, 20, 1, &ok);
    if (!ok) return;
    if (m_model->getWalletUnlockStaking()) {
        Q_EMIT message(tr("Find HD accounts"), tr("The wallet is unlocked for staking only. Lock it, then try again: it asks for the passphrase."), CClientUIInterface::MSG_WARNING);
        return;
    }
    // Unlocked for the whole scan: it derives the keys of the accounts it looks at.
    auto unlock = std::make_unique<WalletModel::UnlockContext>(m_model->requestUnlock());
    if (!unlock->isValid()) return;
    m_scan_unlock = std::move(unlock);

    interfaces::Wallet& wallet = m_model->wallet();
    m_scan_thread = std::thread([this, &wallet, count] {
        std::vector<uint32_t> found;
        bilingual_str error;
        const bool scanned{wallet.rescanHDAccounts(count, found, error)};
        const QString error_text{QString::fromStdString(error.translated)};
        // The page waits for this thread before it goes, so it is still there.
        GUIUtil::ObjectInvoke(this, [this, scanned, found, error_text] { scanFinished(scanned, found, error_text); }, Qt::QueuedConnection);
    });
    updateButtons();
}

void HDAccountsPage::scanFinished(bool ok, const std::vector<uint32_t>& found, const QString& error)
{
    if (m_scan_thread.joinable()) m_scan_thread.join();
    m_scan_unlock.reset();
    if (m_model) Q_EMIT m_model->hdAccountsChanged();
    refresh();
    if (!ok) {
        Q_EMIT message(tr("Find HD accounts"), error, CClientUIInterface::MSG_WARNING);
        return;
    }
    QStringList numbers;
    for (const uint32_t account : found) numbers << QString::number(account);
    Q_EMIT message(tr("Find HD accounts"),
        numbers.isEmpty() ? tr("The scan found no account the wallet did not already hold.") : tr("The scan found these accounts: %1.").arg(numbers.join(", ")),
        CClientUIInterface::MSG_INFORMATION);
}
