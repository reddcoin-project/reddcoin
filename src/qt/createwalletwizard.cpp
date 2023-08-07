// Copyright (c) 2019-2022 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <external_signer.h>
#include <qt/createwalletwizard.h>
#include <qt/forms/ui_createwalletwizard.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <wallet/wallet.h>


#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSpacerItem>
#include <QStringList>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>
#include <QWizardPage>


CreateWalletWizard::CreateWalletWizard(QWidget* parent, SecureString* ssMnemonic_out, SecureString* ssMnemonicPassphrase_out, SecureString* ssMasterKey_out, int* walletType_out) : QWizard(parent, GUIUtil::dialog_flags),
                                                                                                                                                                                    ui(new Ui::CreateWalletWizard),
                                                                                                                                                                                    m_ssMnemonic_out(ssMnemonic_out),
                                                                                                                                                                                    m_ssMnemonicPassphrase_out(ssMnemonicPassphrase_out),
                                                                                                                                                                                    m_ssMasterKey_out(ssMasterKey_out),
                                                                                                                                                                                    m_wallettype_out(walletType_out)
{
    ui->setupUi(this);

    setPage(Page0_walletName, new wizPage_walletName);
    setPage(Page1_walletType, new wizPage_walletType);
    setPage(Page2_walletKeystore, new wizPage_walletKeystore);
    setPage(Page3_walletMasterKey, new wizPage_walletMasterKey);
    setPage(Page4_generateSeed, new wizPage_generateSeed);
    setPage(Page5_confirmSeed, new wizPage_confirmSeed);
    setPage(Page7_enterSeed, new wizPage_enterSeed);
    setPage(Page8_finish, new wizPage_finish);

    setStartId(Page0_walletName);
}

CreateWalletWizard::~CreateWalletWizard()
{
    delete ui;
}

QString CreateWalletWizard::walletName() const
{
    return this->field("wallet.name").toString();
}

bool CreateWalletWizard::isWalletNameEmpty() const
{
    return this->field("wallet.name").toString().isEmpty();
}

bool CreateWalletWizard::isEncryptWalletChecked() const
{
    return this->field("type.encryptWallet").toBool();
}

bool CreateWalletWizard::isDisablePrivateKeysChecked() const
{
    return this->field("type.disablePrivateKeys").toBool();
}

bool CreateWalletWizard::isExternalSignerChecked() const
{
    return this->field("type.externalSigner").toBool();
}

bool CreateWalletWizard::isMakeBlankWalletChecked() const
{
    return this->field("type.blankWallet").toBool();
}

bool CreateWalletWizard::isDescriptorWalletChecked() const
{
    return this->field("type.descriptorWallet").toBool();
}

int CreateWalletWizard::getWalletType() const
{
    return this->field("type.wallet").toInt();
}

void CreateWalletWizard::setSigners(const std::vector<ExternalSigner>& signers)
{
    m_has_signers = !signers.empty();
    if (m_has_signers) {
        this->setField("type.externalSigner", true);
        const std::string label = signers[0].m_name;
        setField("wallet.name", QString::fromStdString(label));
    } else {
        this->setField("type.externalSigner", false);
    }
}

void CreateWalletWizard::accept()
{
    SecureString ssMnemonic;
    SecureString ssMnemonicPassphrase;
    SecureString ssMasterKey;
    int wallettype;

    ssMnemonic.reserve(MAX_PASSPHRASE_SIZE);
    ssMnemonicPassphrase.reserve(MAX_PASSPHRASE_SIZE);
    ssMasterKey.reserve(MAX_PASSPHRASE_SIZE);

    if (!this->field("generateWords.seed").toString().isEmpty()) {
        ssMnemonic.assign(this->field("generateWords.seed").toString().toStdString().c_str());
        ssMnemonicPassphrase.assign(this->field("generateWords.password").toString().toStdString().c_str());
    } else if (!this->field("enter.seed").toString().isEmpty()) {
        ssMnemonic.assign(this->field("enter.seed").toString().toStdString().c_str());
        ssMnemonicPassphrase.assign(this->field("enter.pass").toString().toStdString().c_str());
    }

    if (!this->field("importmaster.key").toString().isEmpty()) {
	ssMasterKey.assign(this->field("importmaster.key").toString().toStdString().c_str());
    }

    m_ssMnemonic_out->assign(ssMnemonic);
    m_ssMnemonicPassphrase_out->assign(ssMnemonicPassphrase);
    m_ssMasterKey_out->assign(ssMasterKey);

    wallettype = this->field("type.wallet").toInt();
    m_wallettype_out = &wallettype;

    QDialog::accept();
}

wizPage_walletName::wizPage_walletName(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Wallet Name"));
    this->setSubTitle(tr("Provide a name for your wallet"));
    verticalLayout = new QVBoxLayout();
    horizontalLayout = new QHBoxLayout();

    wallet_name_label = new QLabel(tr("Wallet Name"));
    wallet_name_line_edit = new QLineEdit();

    lblHelp = new QLabel();

    horizontalLayout->addWidget(wallet_name_label);
    horizontalLayout->addWidget(wallet_name_line_edit);

    verticalLayout->addLayout(horizontalLayout);
    verticalLayout->addWidget(lblHelp);

    verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer);

    setLayout(verticalLayout);

    registerField("wallet.name*", wallet_name_line_edit);
}

void wizPage_walletName::initializePage()
{
    QString walletName = field("wallet.name").toString();
    wallet_name_line_edit->setText(walletName);
}

int wizPage_walletName::nextId() const
{
    if (wallet_name_line_edit->text().isEmpty()) {
        return CreateWalletWizard::Page0_walletName;
    } else {
        return CreateWalletWizard::Page1_walletType;
    }
}

bool wizPage_walletName::pathExist() const
{
    QString filePath = combinePath(GUIUtil::pathToQString(GetWalletDir()), wallet_name_line_edit->text());
    QFile file(filePath);
    return file.exists();
}

QString wizPage_walletName::combinePath(QString dir, QString name) const
{
    return QDir::cleanPath(dir + QDir::separator() + name);
}

bool wizPage_walletName::isComplete() const
{
    if (wallet_name_line_edit->text().isEmpty()) return false;

    if (pathExist()) {
        lblHelp->setText(tr("This wallet already exists\n Choose another name."));
        return false;
    } else {
        lblHelp->setText(tr("This wallet does not exist\n Press 'Next' to create this wallet"));
    }

    return true;
}

wizPage_walletType::wizPage_walletType(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Wallet Type"));
    this->setSubTitle(tr("Select the type of wallet that you want to create along with any advanced options that may be required."));

    verticalLayout = new QVBoxLayout();

    groupBox_1 = new QGroupBox(tr("What kind of wallet do you want to create?"));
    verticalLayout_1 = new QVBoxLayout(groupBox_1);

    buttonGroup = new QButtonGroup();

    radioButton_bip32Wallet = new QRadioButton(tr("Bip32 Wallet"), groupBox_1);
    radioButton_bip32Wallet->setToolTip(tr("Standard HD wallet (No seed phrase)"));
    radioButton_bip39Wallet = new QRadioButton(tr("Bip39 Wallet"), groupBox_1);
    radioButton_bip39Wallet->setToolTip(tr("HD wallet with seed phrase"));
    radioButton_bip39Wallet->setChecked(true);
    radioButton_bip44Wallet = new QRadioButton(tr("Bip44 Wallet"), groupBox_1);
    radioButton_bip44Wallet->setToolTip(tr("HD wallet with seed phrase and coin purpose set."));
    radioButton_blankWallet = new QRadioButton(tr("Blank Wallet"), groupBox_1);
    radioButton_blankWallet->setToolTip(tr("Make a blank wallet. Blank wallets do not initially have private keys or scripts. Private keys and addresses can be imported, or an HD seed can be set, at a later time."));


    verticalLayout_1->addWidget(radioButton_bip32Wallet);
    verticalLayout_1->addWidget(radioButton_bip39Wallet);
    verticalLayout_1->addWidget(radioButton_bip44Wallet);
    verticalLayout_1->addWidget(radioButton_blankWallet);

    walletType = new QLineEdit();
    walletType->setVisible(false);


    buttonGroup->addButton(radioButton_bip32Wallet, walletType::bip32Wallet);
    buttonGroup->addButton(radioButton_bip39Wallet, walletType::bip39Wallet);
    buttonGroup->addButton(radioButton_bip44Wallet, walletType::bip44Wallet);
    buttonGroup->addButton(radioButton_blankWallet, walletType::blankWallet);

    verticalLayout->addWidget(groupBox_1);

    groupBox_2 = new QGroupBox(tr("Encrypt Wallet?"));
    verticalLayout_2 = new QVBoxLayout(groupBox_2);

    checkbox_encryptWallet = new QCheckBox(tr("Encrypt Wallet"), groupBox_2);
    checkbox_encryptWallet->setToolTip(tr("Encrypt the wallet. The wallet will be encrypted with a passphrase of your choice."));

    verticalLayout_2->addWidget(checkbox_encryptWallet);

    verticalLayout->addWidget(groupBox_2);

    groupBox_3 = new QGroupBox(tr("Advanced Options"));
    verticalLayout_3 = new QVBoxLayout(groupBox_3);

    checkBox_disablePrivateKeys = new QCheckBox(tr("Disable Priv Keys"), groupBox_3);
    checkBox_disablePrivateKeys->setToolTip(tr("Disable private keys for this wallet. Wallets with private keys disabled will have no private keys and cannot have an HD seed or imported private keys. This is ideal for watch-only wallets."));
    checkBox_descriptorWallet = new QCheckBox(tr("Descriptor Wallet"), groupBox_3);
    checkBox_descriptorWallet->setToolTip(tr("Use descriptors for scriptPubKey management."));
    checkBox_externalSigner = new QCheckBox(tr("External Signer"), groupBox_3);
    checkBox_externalSigner->setToolTip(tr("Use an external signing device such as a hardware wallet. Configure the external signer script in wallet preferences first."));

    verticalLayout_3->addWidget(checkBox_disablePrivateKeys);
    verticalLayout_3->addWidget(checkBox_descriptorWallet);
    verticalLayout_3->addWidget(checkBox_externalSigner);

    verticalLayout->addWidget(groupBox_3);

    verticalSpacer_3 = new QSpacerItem(20, 238, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer_3);

    setLayout(verticalLayout);

    registerField("type.disablePrivateKeys", checkBox_disablePrivateKeys);
    registerField("type.externalSigner", checkBox_externalSigner);
    registerField("type.encryptWallet", checkbox_encryptWallet);
    registerField("type.blankWallet", radioButton_blankWallet);
    registerField("type.descriptorWallet", checkBox_descriptorWallet);
    registerField("type.wallet", walletType);

    connect(buttonGroup, static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked), [this](int id) {
        setField("type.wallet", id);
    });

    connect(checkbox_encryptWallet, &QCheckBox::toggled, [this](bool checked) {
        // Disable the disable_privkeys_checkbox and external_signer_checkbox when isEncryptWalletChecked is
        // set to true, enable it when isEncryptWalletChecked is false.
        checkBox_disablePrivateKeys->setEnabled(!checked);
#ifdef ENABLE_EXTERNAL_SIGNER
        checkBox_externalSigner->setEnabled(field("type.externalSigner").toBool() && !checked);
#endif
        // When the disable_privkeys_checkbox is disabled, uncheck it.
        if (!checkBox_disablePrivateKeys->isEnabled()) {
            checkBox_disablePrivateKeys->setChecked(false);
        }

        // When the external_signer_checkbox box is disabled, uncheck it.
        if (!checkBox_externalSigner->isEnabled()) {
            checkBox_externalSigner->setChecked(false);
        }
    });

    connect(checkBox_externalSigner, &QCheckBox::toggled, [this](bool checked) {
        checkbox_encryptWallet->setEnabled(!checked);
        radioButton_blankWallet->setEnabled(!checked);
        checkBox_disablePrivateKeys->setEnabled(!checked);
        checkBox_descriptorWallet->setEnabled(!checked);
        radioButton_bip32Wallet->setEnabled(!checked);
        radioButton_bip39Wallet->setEnabled(!checked);
        radioButton_bip44Wallet->setEnabled(!checked);

        // The external signer checkbox is only enabled when a device is detected.
        // In that case it is checked by default. Toggling it restores the other
        // options to their default.
        checkBox_descriptorWallet->setChecked(checked);
        checkbox_encryptWallet->setChecked(false);
        checkBox_disablePrivateKeys->setChecked(checked);
        // The blank check box is ambiguous. This flag is always true for a
        // watch-only wallet, even though we immedidately fetch keys from the
        // external signer.
        radioButton_blankWallet->setChecked(checked);
    });

    connect(checkBox_disablePrivateKeys, &QCheckBox::toggled, [this](bool checked) {
        // Disable the encrypt_wallet_checkbox when isDisablePrivateKeysChecked is
        // set to true, enable it when isDisablePrivateKeysChecked is false.
        checkbox_encryptWallet->setEnabled(!checked);

        // Wallets without private keys start out blank
        if (checked) {
            radioButton_blankWallet->setChecked(true);
        }

        // When the encrypt_wallet_checkbox is disabled, uncheck it.
        if (!checkbox_encryptWallet->isEnabled()) {
            checkbox_encryptWallet->setChecked(false);
        }
    });

    connect(radioButton_blankWallet, &QCheckBox::toggled, [this](bool checked) {
        if (!checked) {
            checkBox_disablePrivateKeys->setChecked(false);
        }
#ifdef ENABLE_EXTERNAL_SIGNER
        checkBox_externalSigner->setEnabled(field("type.externalSigner").toBool() && !checked);
#endif
    });

    connect(radioButton_bip32Wallet, &QCheckBox::toggled, [this](bool checked) {
        if (!checked) {
            checkBox_disablePrivateKeys->setChecked(false);
        }
    });


#ifndef USE_SQLITE
    checkBox_descriptorWallet->setToolTip(tr("Compiled without sqlite support (required for descriptor wallets)"));
    checkBox_descriptorWallet->setEnabled(false);
    checkBox_descriptorWallet->setChecked(false);
    checkBox_externalSigner->setEnabled(false);
    checkBox_externalSigner->setChecked(false);
#endif

#ifndef USE_BDB
    checkBox_descriptorWallet->setEnabled(false);
    checkBox_descriptorWallet->setChecked(true);
#endif

#ifndef ENABLE_EXTERNAL_SIGNER
    //: "External signing" means using devices such as hardware wallets.
    checkBox_externalSigner->setToolTip(tr("Compiled without external signing support (required for external signing)"));
    checkBox_externalSigner->setEnabled(false);
    checkBox_externalSigner->setChecked(false);
#endif
}

int wizPage_walletType::nextId() const
{
    if (radioButton_blankWallet->isChecked()) {
        return CreateWalletWizard::Page8_finish;
    }

    return CreateWalletWizard::Page2_walletKeystore;
}

void wizPage_walletType::initializePage()
{
    setSigners();
    setField("type.wallet", buttonGroup->checkedId());
}

void wizPage_walletType::setSigners()
{
    if (field("type.externalSigner").toBool()) {
        checkBox_externalSigner->setEnabled(true);
        checkBox_externalSigner->setChecked(true);
        checkbox_encryptWallet->setEnabled(false);
        checkbox_encryptWallet->setChecked(false);
        // The order matters, because connect() is called when toggling a checkbox:
        radioButton_blankWallet->setEnabled(false);
        radioButton_blankWallet->setChecked(false);
        checkBox_disablePrivateKeys->setEnabled(false);
        checkBox_disablePrivateKeys->setChecked(true);


        // ui->wallet_name_line_edit->setText(QString::fromStdString(label));
        wizard()->button(QWizard::WizardButton::NextButton)->setEnabled(true);
    } else {
        checkBox_externalSigner->setEnabled(false);
    }
}

wizPage_walletKeystore::wizPage_walletKeystore(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("KeyStore"));
    this->setSubTitle(tr("Do you want to create a new seed, or to restore a wallet using an existing seed/key?"));
    verticalLayout = new QVBoxLayout();

    groupBox_1 = new QGroupBox(tr("Select seed type"));
    verticalLayout_1 = new QVBoxLayout(groupBox_1);

    radioButton_createSeed = new QRadioButton(tr("Create a new seed"), groupBox_1);
    radioButton_createSeed->setChecked(true);
    radioButton_importSeed = new QRadioButton(tr("Already have a seed"), groupBox_1);
    radioButton_masterKey = new QRadioButton(tr("Use a master key"), groupBox_1);
    radioButton_hardware = new QRadioButton(tr("Use a hardware device"), groupBox_1);

    verticalLayout_1->addWidget(radioButton_createSeed);
    verticalLayout_1->addWidget(radioButton_importSeed);
    verticalLayout_1->addWidget(radioButton_masterKey);
    verticalLayout_1->addWidget(radioButton_hardware);

    verticalLayout->addWidget(groupBox_1);

    verticalSpacer_1 = new QSpacerItem(20, 96, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer_1);

    setLayout(verticalLayout);
}

void wizPage_walletKeystore::initializePage()
{
    switch (field("type.wallet").toInt()) {
    case 0: // BIP32
        radioButton_importSeed->hide();
        radioButton_masterKey->show();
        radioButton_hardware->hide();
        break;
    case 1: // BIP39
        radioButton_importSeed->show();
        radioButton_masterKey->hide();
        radioButton_hardware->hide();
        break;
    case 2: // BIP44
        radioButton_importSeed->show();
        radioButton_masterKey->hide();
        radioButton_hardware->hide();
        break;
    case 4: // Blank
        break;
    }
}

int wizPage_walletKeystore::nextId() const
{
    if (radioButton_createSeed->isChecked()) {
        switch (field("type.wallet").toInt()) {
        case 0: // BIP32
            return CreateWalletWizard::Page8_finish;
        case 1: // BIP39
            return CreateWalletWizard::Page4_generateSeed;
        case 2: // BIP44
            return CreateWalletWizard::Page4_generateSeed;
        case 4: // Blank
            return CreateWalletWizard::Page8_finish;
        }
    } else if (radioButton_importSeed->isChecked()) {
        return CreateWalletWizard::Page7_enterSeed;
    } else if (radioButton_masterKey->isChecked()) {
        return CreateWalletWizard::Page3_walletMasterKey;
    } else {
        return CreateWalletWizard::Page4_generateSeed;
    }
    return CreateWalletWizard::Page8_finish;
}

wizPage_walletMasterKey::wizPage_walletMasterKey(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Import MasterKey"));
    this->setSubTitle(tr("Please enter your masterkey in order to restore your wallet."));
    verticalLayout = new QVBoxLayout();

    label_12 = new QLabel(tr("The WIF private key to use as the new HD seed.\nThe seed value can be retrieved using the dumpwallet command or gethdwalletinfo.\nIt is the private key marked hdseed=1."));
    label_12->setWordWrap(true);

    verticalLayout->addWidget(label_12);

    textEdit_importMasterKey = new QTextEdit();
    QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    sizePolicy.setHeightForWidth(textEdit_importMasterKey->sizePolicy().hasHeightForWidth());
    textEdit_importMasterKey->setSizePolicy(sizePolicy);
    textEdit_importMasterKey->setMaximumSize(QSize(16777215, 80));

    verticalLayout->addWidget(textEdit_importMasterKey);

    verticalSpacer = new QSpacerItem(20, 130, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer);

    setLayout(verticalLayout);

    registerField("importmaster.key", textEdit_importMasterKey, "plainText");
}

int wizPage_walletMasterKey::nextId() const
{
    return -1;
}

wizPage_generateSeed::wizPage_generateSeed(QWidget* parent)
    : QWizardPage(parent),
      fCapsLock(false)
{
    this->setTitle(tr("Generate Seed"));
    this->setSubTitle(tr("The seed automatically generated for your wallet"));

    verticalLayout = new QVBoxLayout();

    lblHeading = new QLabel(tr("Your wallet generation seed is:"));

    verticalLayout->addWidget(lblHeading);

    textEdit_newMnemonic = new QTextEdit();

    QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    sizePolicy.setHeightForWidth(textEdit_newMnemonic->sizePolicy().hasHeightForWidth());
    textEdit_newMnemonic->setSizePolicy(sizePolicy);
    textEdit_newMnemonic->setMinimumSize(QSize(0, 0));
    textEdit_newMnemonic->setMaximumSize(QSize(16777215, 80));

    verticalLayout->addWidget(textEdit_newMnemonic);

    lblHelp = new QLabel();
    lblHelp->setWordWrap(true);
    lblHelp->setMinimumSize(QSize(600, 0));
    lblHelp->setMaximumSize(QSize(700, 50));

    horizontalLayout1 = new QHBoxLayout();

    horizontalLayout1->addWidget(lblHelp);

    horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    horizontalLayout1->addItem(horizontalSpacer);

    label_13 = new QLabel(tr("Words"));
    horizontalLayout1->addWidget(label_13);
    comboBox = new QComboBox();
    QString wordCount = "12,15,18,21,24";
    QStringList wordCountList = wordCount.split(",");
    comboBox->addItems(wordCountList);

    horizontalLayout1->addWidget(comboBox);

    label_14 = new QLabel(tr("Language"));
    horizontalLayout1->addWidget(label_14);
    comboBoxLanguage = new QComboBox();
    std::array<LanguageDetails, NUM_LANGUAGES_BIP39_SUPPORTED> languagesDetails = CMnemonic::GetLanguagesDetails();
    for (int langNum = 0; langNum < NUM_LANGUAGES_BIP39_SUPPORTED; langNum++) {
        comboBoxLanguage->addItem(languagesDetails[langNum].label);
    }

    comboBoxLanguage->installEventFilter(this);

    horizontalLayout1->addWidget(comboBoxLanguage);

    verticalLayout->addLayout(horizontalLayout1);

    horizontalLayout0 = new QHBoxLayout();

    label_3 = new QLabel(tr("Pass Phrase"));
    horizontalLayout0->addWidget(label_3);

    lineEdit_password = new QLineEdit();
    lineEdit_password->setEchoMode(QLineEdit::Password);
    lineEdit_password->installEventFilter(this);
    horizontalLayout0->addWidget(lineEdit_password);

    checkBox_showPassword = new QCheckBox(tr("Show passphrase"));
    horizontalLayout0->addWidget(checkBox_showPassword);

    verticalLayout->addLayout(horizontalLayout0);

    label_5 = new QLabel(tr("Please save these words on paper (order is important). This seed will allow you to recover your wallet in case of computer failure."));
    label_5->setWordWrap(true);

    verticalLayout->addWidget(label_5);

    label_6 = new QLabel(tr("Warning:"));

    QFont font;
    font.setBold(true);
    font.setWeight(75);
    label_6->setFont(font);

    verticalLayout->addWidget(label_6);

    label_7 = new QLabel(tr("<html><head/><body><ul style=\"margin-top: 0px; margin-bottom: 0px; margin-left: 0px; margin-right: 0px; -qt-list-indent: 1;\"><li style=\" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\">Never disclose your seed. </li><li style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\">Never type it on a website.</li><li style=\" margin-top:0px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\">Do not store it electronically</li></ul></body></html>"));

    label_7->setTextFormat(Qt::AutoText);
    label_7->setWordWrap(true);

    verticalLayout->addWidget(label_7);

    verticalSpacer = new QSpacerItem(20, 101, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer);

    setLayout(verticalLayout);

    registerField("generateWords.seed", textEdit_newMnemonic, "plainText");
    registerField("generateWords.password", lineEdit_password);
    connect(textEdit_newMnemonic, SIGNAL(textChanged()), this, SIGNAL(completeChanged()));
    connect(comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index){
        onChangeWords(index);
    });
    connect(comboBoxLanguage, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index){
        onChangeLanguage(index);
    });
    connect(checkBox_showPassword, &QCheckBox::toggled, [this](bool checked) {
        toggleShowPassword(checked);
    });
}

wizPage_generateSeed::~wizPage_generateSeed()
{
    secureClearPassFields();
}

void wizPage_generateSeed::secureClearPassFields()
{
    SecureClearQLineEdit(lineEdit_password);
}

void wizPage_generateSeed::toggleShowPassword(bool show)
{
    checkBox_showPassword->setDown(show);
    const auto mode = show ? QLineEdit::Normal : QLineEdit::Password;
    lineEdit_password->setEchoMode(mode);
}

int wizPage_generateSeed::getStrength(int idx = 0)
{
    int strength[] = {128, 160, 192, 224, 256};
    return strength[idx];
}

void wizPage_generateSeed::generateWords(int strength, int languageSelected)
{
    SecureString words = CMnemonic::Generate(strength, languageSelected);
    std::string str_words = std::string(words.begin(), words.end());
    textEdit_newMnemonic->setText(QString::fromStdString(str_words));
}

void wizPage_generateSeed::onChangeWords(int idx)
{
  int strength = getStrength(idx);
  int languageSelected = comboBoxLanguage->currentIndex();

  generateWords(strength, languageSelected);
}

void wizPage_generateSeed::onChangeLanguage(int languageSelected)
{
  int strength = getStrength(comboBox->currentIndex());
  generateWords(strength, languageSelected);
}

bool wizPage_generateSeed::event(QEvent* event)
{
    // Detect Caps Lock key press.
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_CapsLock) {
            fCapsLock = !fCapsLock;
        }
        if (fCapsLock) {
            QFont font;
            font.setBold(true);
            font.setWeight(75);
            lblHelp->setFont(font);
            lblHelp->setText(tr("Warning: The Caps Lock key is on!"));
        } else {
            lblHelp->clear();
        }
    }
    return QWidget::event(event);
}

bool wizPage_generateSeed::eventFilter(QObject* object, QEvent* event)
{
    /* Detect Caps Lock.
     * There is no good OS-independent way to check a key state in Qt, but we
     * can detect Caps Lock by checking for the following condition:
     * Shift key is down and the result is a lower case character, or
     * Shift key is not down and the result is an upper case character.
     */
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        QString str = ke->text();
        if (str.length() != 0) {
            const QChar* psz = str.unicode();
            bool fShift = (ke->modifiers() & Qt::ShiftModifier) != 0;
            if ((fShift && *psz >= 'a' && *psz <= 'z') || (!fShift && *psz >= 'A' && *psz <= 'Z')) {
                fCapsLock = true;
                QFont font;
                font.setBold(true);
                font.setWeight(75);
                lblHelp->setFont(font);
                lblHelp->setText(tr("Warning: The Caps Lock key is on!"));
            } else if (psz->isLetter()) {
                fCapsLock = false;
                lblHelp->clear();
            }
        }
    }
    return QWizardPage::eventFilter(object, event);
}

void wizPage_generateSeed::initializePage()
{
    generateWords(getStrength(comboBox->currentIndex()), comboBoxLanguage->currentIndex());
}

bool wizPage_generateSeed::isComplete() const
{
    std::string words = textEdit_newMnemonic->toPlainText().toStdString();
    std::string passphrase = lineEdit_password->text().toStdString();

    SecureString my_words(words.begin(), words.end());
    SecureString my_passphrase(passphrase.begin(), passphrase.end());

    QFont font;
    font.setBold(true);
    font.setWeight(75);
    lblHelp->setFont(font);

    lblHelp->setText("");

    if (words.find("\u3000") != std::string::npos) {
	lblHelp->setText(tr("Please use standard space, the use of ideographic japanese spaces is not supported"));
        my_words.clear();
        my_passphrase.clear();
	return false;
    };

    // NOTE: default mnemonic passphrase is an empty string
    if (!CMnemonic::Check(my_words, comboBoxLanguage->currentIndex())) {
        lblHelp->setText(tr("Words are not valid, please check the words and language and try again"));
        my_words.clear();
        my_passphrase.clear();
        return false;
    }

    return true;
}

int wizPage_generateSeed::nextId() const
{
    return CreateWalletWizard::Page5_confirmSeed;
}

wizPage_confirmSeed::wizPage_confirmSeed(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Confirm Seed"));
    this->setSubTitle(tr("Your seed is important!\n"
                         "If you lose your seed, your money will be permanently lost.\n"
                         "To make sure that you have properly saved your seed, please retype it here."));

    verticalLayout = new QVBoxLayout();

    label_10 = new QLabel(tr("Enter your seed words"));
    label_10->setWordWrap(true);

    verticalLayout->addWidget(label_10);

    textEdit_confirmMnemonic = new QTextEdit();
    QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    sizePolicy.setHeightForWidth(textEdit_confirmMnemonic->sizePolicy().hasHeightForWidth());
    textEdit_confirmMnemonic->setSizePolicy(sizePolicy);
    textEdit_confirmMnemonic->setMinimumSize(QSize(0, 0));
    textEdit_confirmMnemonic->setMaximumSize(QSize(16777215, 80));

    verticalLayout->addWidget(textEdit_confirmMnemonic);

    verticalSpacer_5 = new QSpacerItem(20, 96, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer_5);

    setLayout(verticalLayout);

    registerField("confirm.seed", textEdit_confirmMnemonic, "plainText");
    connect(textEdit_confirmMnemonic, SIGNAL(textChanged()), this, SIGNAL(completeChanged()));
}

bool wizPage_confirmSeed::isComplete() const
{
    if (QString(field("generateWords.seed").toString()) == QString(field("confirm.seed").toString())) {
        return true;
    }
    return false;
}

int wizPage_confirmSeed::nextId() const
{
    return CreateWalletWizard::Page8_finish;
}

wizPage_enterSeed::wizPage_enterSeed(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Enter Seed"));
    this->setSubTitle(tr("Please enter your seed phrase in order to restore your wallet."));
    verticalLayout = new QVBoxLayout();

    label_9 = new QLabel(tr("Enter your seed phrase."));
    label_9->setWordWrap(true);

    verticalLayout->addWidget(label_9);

    textEdit_importMnemonic = new QTextEdit();
    QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sizePolicy.setHorizontalStretch(0);
    sizePolicy.setVerticalStretch(0);
    sizePolicy.setHeightForWidth(textEdit_importMnemonic->sizePolicy().hasHeightForWidth());
    textEdit_importMnemonic->setSizePolicy(sizePolicy);
    textEdit_importMnemonic->setMaximumSize(QSize(16777215, 80));

    verticalLayout->addWidget(textEdit_importMnemonic);

    lblHelp = new QLabel();
    lblHelp->setWordWrap(true);
    lblHelp->setMinimumSize(QSize(600, 0));
    lblHelp->setMaximumSize(QSize(700, 50));

    horizontalLayout = new QHBoxLayout();

    label_3 = new QLabel(tr("Pass Phrase"));


    horizontalLayout->addWidget(label_3);

    lineEdit_password = new QLineEdit();

    horizontalLayout->addWidget(lineEdit_password);

    verticalLayout->addLayout(horizontalLayout);
    verticalLayout->addWidget(lblHelp);


    verticalSpacer_2 = new QSpacerItem(20, 130, QSizePolicy::Minimum, QSizePolicy::Expanding);

    verticalLayout->addItem(verticalSpacer_2);

    setLayout(verticalLayout);

    registerField("enter.seed", textEdit_importMnemonic, "plainText");
    registerField("enter.pass", lineEdit_password);

    connect(textEdit_importMnemonic, SIGNAL(textChanged()), this, SIGNAL(completeChanged()));
}

int wizPage_enterSeed::nextId() const
{
    return CreateWalletWizard::Page8_finish;
}

bool wizPage_enterSeed::isComplete() const
{
    std::string words = textEdit_importMnemonic->toPlainText().toStdString();
    std::string passphrase = lineEdit_password->text().toStdString();

    SecureString my_words(words.begin(), words.end());
    SecureString my_passphrase(passphrase.begin(), passphrase.end());

    lblHelp->setText("");

    if (my_words.empty()) {
        return false;
    }

    // NOTE: default mnemonic passphrase is an empty string
    if (!CMnemonic::Check(my_words)) {
        lblHelp->setText(tr("<b>Words are not valid, please check the words and try again</b>"));
        my_words.clear();
        my_passphrase.clear();
        return false;
    }

    return true;
}

wizPage_finish::wizPage_finish(QWidget* parent)
    : QWizardPage(parent)
{
    this->setTitle(tr("Create Wallet"));
    this->setSubTitle(tr("Click 'Finish' to complete creating your wallet."));
}

int wizPage_finish::nextId() const
{
    return -1;
}

static void SecureClearQLineEdit(QLineEdit* edit)
{
    // Attempt to overwrite text so that they do not linger around in memory
    edit->setText(QString(" ").repeated(edit->text().size()));
    edit->clear();
}
