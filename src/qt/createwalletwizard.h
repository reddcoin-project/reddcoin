// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CREATEWALLETWIZARD_H
#define BITCOIN_QT_CREATEWALLETWIZARD_H

#include <QWizard>

#include <support/allocators/secure.h>

class ExternalSigner;
class WalletModel;

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpacerItem;
class QTextEdit;
class QVBoxLayout;
QT_END_NAMESPACE

namespace Ui {
class CreateWalletWizard;
} // namespace Ui

static void SecureClearQLineEdit(QLineEdit* edit);

/** Dialog for creating wallets
 */
class CreateWalletWizard : public QWizard
{
    Q_OBJECT

public:
    enum { Page0_walletName,
           Page1_walletType,
           Page2_walletKeystore,
           Page3_walletMasterKey,
           Page4_generateSeed,
           Page5_confirmSeed,
           Page7_enterSeed,
           Page8_finish };
    explicit CreateWalletWizard(QWidget* parent, SecureString* m_ssMnemonic_out = nullptr, SecureString* m_ssMnemonicPassphrase_out = nullptr, SecureString* m_ssMasterKey_out = nullptr, int* m_wallettype_out = nullptr);
    virtual ~CreateWalletWizard();

    void accept() override;

    void setSigners(const std::vector<ExternalSigner>& signers);
    //
    QString walletName() const;
    bool isEncryptWalletChecked() const;
    bool isWalletNameEmpty() const;
    bool isDisablePrivateKeysChecked() const;
    bool isMakeBlankWalletChecked() const;
    bool isDescriptorWalletChecked() const;
    bool isExternalSignerChecked() const;
    int getWalletType() const;

private:
    Ui::CreateWalletWizard* ui;
    bool m_has_signers = false;
    SecureString* m_ssMnemonic_out;
    SecureString* m_ssMnemonicPassphrase_out;
    SecureString* m_ssMasterKey_out;
    int* m_wallettype_out;
};


class wizPage_walletName : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_walletName(QWidget* parent = nullptr);

    bool pathExist() const;
    QString combinePath(QString dir, QString name) const;
    void initializePage() override;
    int nextId() const override;


private:
    QVBoxLayout* verticalLayout;
    QHBoxLayout* horizontalLayout;
    QLabel* wallet_name_label;
    QLabel* lblHelp;
    QLineEdit* wallet_name_line_edit;
    QSpacerItem* verticalSpacer;

    bool isComplete() const override;
};

class wizPage_walletType : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_walletType(QWidget* parent = nullptr);
    void setSigners();

    int nextId() const override;
    void initializePage() override;

private:
    QVBoxLayout* verticalLayout;
    QLabel* wallet_type_label;
    QGroupBox* groupBox_1;
    QVBoxLayout* verticalLayout_1;
    QRadioButton* radioButton_bip32Wallet;
    QRadioButton* radioButton_bip39Wallet;
    QRadioButton* radioButton_bip44Wallet;
    QRadioButton* radioButton_blankWallet;
    QButtonGroup* buttonGroup;
    QLineEdit* walletType;
    QGroupBox* groupBox_2;
    QVBoxLayout* verticalLayout_2;
    QCheckBox* checkbox_encryptWallet;
    QGroupBox* groupBox_3;
    QVBoxLayout* verticalLayout_3;
    QCheckBox* checkBox_disablePrivateKeys;
    QCheckBox* checkBox_descriptorWallet;
    QCheckBox* checkBox_externalSigner;
    QSpacerItem* verticalSpacer_3;
};

class wizPage_walletKeystore : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_walletKeystore(QWidget* parent = nullptr);

    void initializePage() override;
    int nextId() const override;

private:
    QVBoxLayout* verticalLayout;
    QGroupBox* groupBox_1;
    QVBoxLayout* verticalLayout_1;
    QRadioButton* radioButton_createSeed;
    QRadioButton* radioButton_importSeed;
    QRadioButton* radioButton_masterKey;
    QRadioButton* radioButton_hardware;
    QSpacerItem* verticalSpacer_1;
};

class wizPage_walletMasterKey : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_walletMasterKey(QWidget* parent = nullptr);

    int nextId() const override;

private:
    QVBoxLayout* verticalLayout;
    QLabel* label_12;
    QTextEdit* textEdit_importMasterKey;
    QSpacerItem* verticalSpacer;
};


class wizPage_generateSeed : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_generateSeed(QWidget* parent = nullptr);
    ~wizPage_generateSeed();


    // void generateWords();
    //    void generateWords(int value);

    void initializePage() override;
    int nextId() const override;

private:
    QVBoxLayout* verticalLayout;
    QLabel* lblHeading;
    QTextEdit* textEdit_newMnemonic;
    QHBoxLayout* horizontalLayout0;
    QLabel* label_3;
    QLineEdit* lineEdit_password;
    QLabel* label_4;
    QCheckBox* checkBox_showPassword;
    QLabel* label_5;
    QLabel* label_6;
    QLabel* label_7;
    QSpacerItem* verticalSpacer;
    QLabel* lblHelp;
    QHBoxLayout* horizontalLayout1;
    QSpacerItem* horizontalSpacer;
    QLabel* label_13;
    QComboBox* comboBox;
    QLabel* label_14;
    QComboBox* comboBoxLanguage;

    bool fCapsLock;

    bool isComplete() const override;
    int getStrength(int idx);
    void secureClearPassFields();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private Q_SLOTS:
    void generateWords(int strength, int languageSelected);
    void onChangeWords(int idx);
    void onChangeLanguage(int idx);
    void toggleShowPassword(bool);
};

class wizPage_confirmSeed : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_confirmSeed(QWidget* parent = nullptr);

    int nextId() const override;

private:
    QVBoxLayout* verticalLayout;
    QLabel* label_10;
    QTextEdit* textEdit_confirmMnemonic;
    QSpacerItem* verticalSpacer_5;

    bool isComplete() const override;
};

class wizPage_enterSeed : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_enterSeed(QWidget* parent = nullptr);

    int nextId() const override;

private:
    QVBoxLayout* verticalLayout;
    QLabel* label_8;
    QLabel* label_9;
    QTextEdit* textEdit_importMnemonic;
    QHBoxLayout* horizontalLayout;
    QLabel* label_3;
    QLineEdit* lineEdit_password;
    QLabel* lblHelp;
    QSpacerItem* verticalSpacer_2;

    bool isComplete() const override;
};

class wizPage_finish : public QWizardPage
{
    Q_OBJECT

public:
    wizPage_finish(QWidget* parent = nullptr);

    int nextId() const override;

private:
};
#endif // BITCOIN_QT_CREATEWALLETWIZARD_H
