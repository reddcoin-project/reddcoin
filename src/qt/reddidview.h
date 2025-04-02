#ifndef BITCOIN_QT_REDDIDVIEW_H
#define BITCOIN_QT_REDDIDVIEW_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QScrollArea>
#include <QLineEdit>
#include <QComboBox>
#include <QGroupBox>
#include <memory>

class PlatformStyle;
class WalletModel;
class ClientModel;
class OptionsModel;

namespace Ui {
    class ReddIDView;
}

/** ReddID dashboard widget (ReddID tab) */
class ReddIDView : public QWidget
{
    Q_OBJECT

public:
    explicit ReddIDView(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~ReddIDView();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);

private:
    Ui::ReddIDView *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;
    const PlatformStyle *platformStyle;

    QVBoxLayout *mainLayout;
    QLabel *headerLabel;
    QLabel *statusLabel;
    QTabWidget *dashboardTabs;
    
    // Dashboard sections
    QWidget *summaryWidget;
    QWidget *namespacesWidget;
    QWidget *userIDsWidget;
    QWidget *profileWidget;
    QWidget *socialWidget;
    
    // Summary section elements
    QLabel *summaryInfoLabel;
    QPushButton *refreshButton;
    
    void setupUI();
    void setupSummaryTab();
    void setupNamespacesTab();
    void setupUserIDsTab();
    void setupProfileTab();
    void setupSocialTab();
    void updateUI();

private Q_SLOTS:
    void refreshButtonClicked();
    void showProfileTab();
    void showNamespacesTab();
    void showUserIDsTab();
    void showSocialTab();
};

#endif // BITCOIN_QT_REDDIDVIEW_H