// Copyright (c) 2018-2020 The Bitcoin Core developers
// Copyright (c) 2018-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_WALLET_H
#define BITCOIN_INTERFACES_WALLET_H

#include <amount.h>                    // For CAmount
#include <interfaces/chain.h>          // For ChainClient
#include <pubkey.h>                    // For CKeyID and CScriptID (definitions needed in CTxDestination instantiation)
#include <script/standard.h>           // For CTxDestination
#include <support/allocators/secure.h> // For SecureString
#include <util/message.h>
#include <util/translation.h>
#include <util/ui_change_type.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdint.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class CCoinControl;
class CFeeRate;
class CKey;
class CWallet;
enum class FeeReason;
enum class OutputType;
enum class TransactionError;
enum isminetype : unsigned int;
struct CRecipient;
struct PartiallySignedTransaction;
struct WalletContext;
struct bilingual_str;
typedef uint8_t isminefilter;

namespace interfaces {

class Handler;
struct WalletAddress;
struct WalletBalances;
struct WalletHDAccount;
struct WalletHDAccountsState;
struct WalletRestoreReport;
struct WalletTx;
struct WalletMintingOutput;
struct WalletTxOut;
struct WalletTxStatus;

using WalletOrderForm = std::vector<std::pair<std::string, std::string>>;
using WalletValueMap = std::map<std::string, std::string>;

//! Interface for accessing a wallet.
class Wallet
{
public:
    virtual ~Wallet() {}

    //! Encrypt wallet.
    virtual bool encryptWallet(const SecureString& wallet_passphrase) = 0;

    //! Return whether wallet is encrypted.
    virtual bool isCrypted() = 0;

    //! Lock wallet.
    virtual bool lock() = 0;

    //! Unlock wallet.
    virtual bool unlock(const SecureString& wallet_passphrase) = 0;

    //! Whether the next unlock moves the seed into its encrypted record or finishes a rewrite of the wallet file.
    virtual bool hdSecretsRewriteOnUnlock() = 0;

    //! What the owner is to be told after the seed was moved and the file rewritten; given once.
    virtual std::vector<bilingual_str> takeHDSecretsMovedNotices() = 0;

    //! What the owner has to be told about where the seed of the wallet is kept, if anything.
    //! with_advice: the form given at load, with what to do.
    virtual std::vector<bilingual_str> getHDWarnings(bool with_advice) = 0;

    //! Whether the wallet holds keys its seed does not restore, so that only its file does.
    virtual bool hasKeysSeedDoesNotRestore() = 0;

    //! Whether the wallet holds keys of HD accounts above 0 without the flag
    //! that says it holds accounts, as an earlier wallet tool's salvage leaves it.
    virtual bool hdAccountsInconsistent() = 0;

    //! Whether the scan of a wallet restored from its seed is still owed.
    virtual bool hdRestoreScanOwed() = 0;

    //! Whether the wallet is locked with addresses of its HD accounts still to
    //! be derived or found (WalletHDAccountsState::unlock_owed), cheaply.
    virtual bool hdAccountsUnlockOwed() = 0;

    //! What the GUI shows and allows for the HD accounts of the wallet.
    virtual WalletHDAccountsState getHDAccountsState() = 0;

    //! Account 0 and the accounts handed out, active and retired; not the
    //! accounts kept ahead. Balances and use only if asked: they take a pass
    //! over the wallet's transactions.
    virtual std::vector<WalletHDAccount> listHDAccounts(bool with_balances) = 0;

    //! Create the next HD account (newhdaccount).
    virtual bool newHDAccount(const std::string& label, bool force, uint32_t& account, CTxDestination& first,
        std::vector<bilingual_str>& warnings, bilingual_str& error) = 0;

    //! Change the label of an HD account, or retire it or bring it back (sethdaccount).
    virtual bool setHDAccount(uint32_t account, const std::optional<std::string>& label, std::optional<bool> retired,
        bilingual_str& error) = 0;

    //! The receive addresses of an HD account above 0 handed out and not yet paid.
    virtual std::vector<CTxDestination> getUnpaidHDAccountAddresses(uint32_t account) = 0;

    //! Scan the whole block chain for the HD accounts of the seed (rescanhdaccounts).
    //! found: the accounts this scan found.
    virtual bool rescanHDAccounts(int count, std::vector<uint32_t>& found, bilingual_str& error) = 0;

    //! A new receive address of an HD account; account 0 is getNewDestination's.
    virtual bool getNewHDAccountDestination(uint32_t account, OutputType type, const std::string& label,
        CTxDestination& dest, bilingual_str& error) = 0;

    //! The HD account of a script of the wallet; 0 for any other script.
    virtual uint32_t getHDAccountOfScript(const CScript& script) = 0;

    //! What a restore from a phrase found, for the report after the wizard.
    virtual WalletRestoreReport getRestoreReport() = 0;

    //! Return whether wallet is locked.
    virtual bool isLocked() = 0;

    //! Change wallet passphrase.
    virtual bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) = 0;

    //! Abort a rescan.
    virtual void abortRescan() = 0;

    //! Back up wallet.
    virtual bool backupWallet(const std::string& filename) = 0;

    //! Get wallet name.
    virtual std::string getWalletName() = 0;

    // Get a new address.
    virtual bool getNewDestination(const OutputType type, const std::string label, CTxDestination& dest) = 0;

    //! Get public key.
    virtual bool getPubKey(const CScript& script, const CKeyID& address, CPubKey& pub_key) = 0;

    //! Stake weight the staking thread last published for this wallet, in
    //! coin-days: average per coin and total. Returns false, with zeros,
    //! until a search pass has completed or once the thread has stopped.
    //! Takes no lock.
    virtual bool getStakeWeight(uint64_t& average_weight, uint64_t& total_weight) = 0;

    //! Get wallet staking enabled.
    virtual bool getEnableStaking() = 0;

    //! Set wallet staking enabled.
    virtual void setEnableStaking(bool enableStaking) = 0;

    //! Get wallet unlocked for staking.
    virtual bool getUnlockWalletStaking() = 0;

    //! Set wallet unlock for staking.
    virtual void setUnlockWalletStaking(bool stakingOnly) = 0;

    //! Get Last CoinStake SearchInterval.
    virtual int64_t getLastCoinStakeSearchInterval() = 0;

    //! Sign message
    virtual SigningResult signMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) = 0;

    //! Return whether wallet has private key.
    virtual bool isSpendable(const CTxDestination& dest) = 0;

    //! Return whether wallet has watch only keys.
    virtual bool haveWatchOnly() = 0;

    //! Add or update address.
    virtual bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) = 0;

    // Remove address.
    virtual bool delAddressBook(const CTxDestination& dest) = 0;

    //! Look up address in wallet, return whether exists.
    virtual bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) = 0;

    //! Get wallet address list.
    virtual std::vector<WalletAddress> getAddresses() = 0;

    //! Get receive requests.
    virtual std::vector<std::string> getAddressReceiveRequests() = 0;

    //! Save or remove receive request.
    virtual bool setAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& value) = 0;

    //! Display address on external signer
    virtual bool displayAddress(const CTxDestination& dest) = 0;

    //! Lock coin.
    virtual void lockCoin(const COutPoint& output) = 0;

    //! Unlock coin.
    virtual void unlockCoin(const COutPoint& output) = 0;

    //! Return whether coin is locked.
    virtual bool isLockedCoin(const COutPoint& output) = 0;

    //! List locked coins.
    virtual void listLockedCoins(std::vector<COutPoint>& outputs) = 0;

    //! Create transaction.
    virtual CTransactionRef createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        bilingual_str& fail_reason) = 0;

    //! Commit transaction.
    virtual void commitTransaction(CTransactionRef tx,
        WalletValueMap value_map,
        WalletOrderForm order_form) = 0;

    //! Return whether transaction can be abandoned.
    virtual bool transactionCanBeAbandoned(const uint256& txid) = 0;

    //! Abandon transaction.
    virtual bool abandonTransaction(const uint256& txid) = 0;

    //! Return whether transaction can be bumped.
    virtual bool transactionCanBeBumped(const uint256& txid) = 0;

    //! Create bump transaction.
    virtual bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        std::vector<bilingual_str>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) = 0;

    //! Sign bump transaction.
    virtual bool signBumpTransaction(CMutableTransaction& mtx) = 0;

    //! Commit bump transaction.
    virtual bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<bilingual_str>& errors,
        uint256& bumped_txid) = 0;

    //! Get a transaction.
    virtual CTransactionRef getTx(const uint256& txid) = 0;

    //! Get transaction information.
    virtual WalletTx getWalletTx(const uint256& txid) = 0;

    //! Get list of all wallet transactions.
    virtual std::vector<WalletTx> getWalletTxs() = 0;

    //! Try to get updated status for a particular transaction, if possible without blocking.
    virtual bool tryGetTxStatus(const uint256& txid,
        WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& block_time) = 0;

    //! Get transaction details.
    virtual WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks) = 0;

    //! Fill PSBT.
    virtual TransactionError fillPSBT(int sighash_type,
        bool sign,
        bool bip32derivs,
        size_t* n_signed,
        PartiallySignedTransaction& psbtx,
        bool& complete) = 0;

    //! Get balances.
    virtual WalletBalances getBalances() = 0;

    //! Get balances if possible without blocking.
    virtual bool tryGetBalances(WalletBalances& balances, uint256& block_hash) = 0;

    //! Get balance.
    virtual CAmount getBalance() = 0;

    //! Get available balance.
    virtual CAmount getAvailableBalance(const CCoinControl& coin_control) = 0;

    //! Return whether transaction input belongs to wallet.
    virtual isminetype txinIsMine(const CTxIn& txin) = 0;

    //! Return whether transaction output belongs to wallet.
    virtual isminetype txoutIsMine(const CTxOut& txout) = 0;

    //! Return debit amount if transaction input belongs to wallet.
    virtual CAmount getDebit(const CTxIn& txin, isminefilter filter) = 0;

    //! Return credit amount if transaction input belongs to wallet.
    virtual CAmount getCredit(const CTxOut& txout, isminefilter filter) = 0;

    //! Return AvailableCoins + LockedCoins of one HD account grouped by wallet
    //! address (put change in one group with wallet address).
    using CoinsList = std::map<CTxDestination, std::vector<std::tuple<COutPoint, WalletTxOut>>>;
    virtual CoinsList listCoins(uint32_t account) = 0;

    //! Return wallet transaction output information.
    virtual std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) = 0;

    //! Return the outputs the minting table lists: every unspent output of
    //! the wallet whose transaction is confirmed (twice, for a coinbase).
    //! Unlike listCoins() this includes immature stake and locked coins.
    virtual std::vector<WalletMintingOutput> getMintingOutputs() = 0;

    //! Return the same for one transaction and for each wallet transaction
    //! it spends from. txids receives every transaction the answer covers,
    //! including those with nothing left to list.
    virtual std::vector<WalletMintingOutput> getMintingOutputs(const uint256& txid, std::vector<uint256>& txids) = 0;

    //! Get required fee.
    virtual CAmount getRequiredFee(unsigned int tx_bytes) = 0;

    //! Get minimum fee.
    virtual CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) = 0;

    //! Get tx confirm target.
    virtual unsigned int getConfirmTarget() = 0;

    // Return whether HD enabled.
    virtual bool hdEnabled() = 0;

    // Return whether BIP39 enabled.
    virtual bool bip39Enabled() = 0;

    // Return whether BIP44 enabled.
    virtual bool bip44Enabled() = 0;

    // Return whether the wallet is blank.
    virtual bool canGetAddresses() = 0;

    // Return whether private keys enabled.
    virtual bool privateKeysDisabled() = 0;

    // Return whether wallet uses an external signer.
    virtual bool hasExternalSigner() = 0;

    // Get default address type.
    virtual OutputType getDefaultAddressType() = 0;

    //! Get max tx fee.
    virtual CAmount getDefaultMaxTxFee() = 0;

    // Remove wallet.
    virtual void remove() = 0;

    //! Return whether is a legacy wallet
    virtual bool isLegacy() = 0;

    //! Register handler for unload message.
    using UnloadFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleUnload(UnloadFn fn) = 0;

    //! Register handler for show progress messages.
    using ShowProgressFn = std::function<void(const std::string& title, int progress)>;
    virtual std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) = 0;

    //! Register handler for status changed messages.
    using StatusChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) = 0;

    //! Register handler for wallet staking status messages.
    using NotifyWalletStakingStatusChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleNotifyWalletStakingStatusChanged(NotifyWalletStakingStatusChangedFn fn) = 0;

    //! Register handler for address book changed messages.
    using AddressBookChangedFn = std::function<void(const CTxDestination& address,
        const std::string& label,
        bool is_mine,
        const std::string& purpose,
        ChangeType status)>;
    virtual std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) = 0;

    //! Register handler for transaction changed messages.
    using TransactionChangedFn = std::function<void(const uint256& txid, ChangeType status)>;
    virtual std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) = 0;

    //! Register handler for watchonly changed messages.
    using WatchOnlyChangedFn = std::function<void(bool have_watch_only)>;
    virtual std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) = 0;

    //! Register handler for keypool changed messages.
    using CanGetAddressesChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) = 0;

    //! Register handler for HD accounts changed messages.
    using HDAccountsChangedFn = std::function<void()>;
    virtual std::unique_ptr<Handler> handleHDAccountsChanged(HDAccountsChangedFn fn) = 0;

    //! Return pointer to internal wallet class, useful for testing.
    virtual CWallet* wallet() { return nullptr; }
};

//! Wallet chain client that in addition to having chain client methods for
//! starting up, shutting down, and registering RPCs, also has additional
//! methods (called by the GUI) to load and create wallets.
class WalletClient : public ChainClient
{
public:
    //! Create new wallet.
    virtual std::unique_ptr<Wallet> createWallet(const std::string& name, const SecureString& ssMnemonic, const SecureString& ssMnemonicPassphrase, const SecureString& ssMasterKey, const SecureString& passphrase, const int& walletType, const bool& importing, uint64_t wallet_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings) = 0;

   //! Load existing wallet.
   virtual std::unique_ptr<Wallet> loadWallet(const std::string& name, bilingual_str& error, std::vector<bilingual_str>& warnings) = 0;

   //! Return default wallet directory.
   virtual std::string getWalletDir() = 0;

   //! Return available wallets in wallet directory.
   virtual std::vector<std::string> listWalletDir() = 0;

   //! Return interfaces for accessing wallets (if any).
   virtual std::vector<std::unique_ptr<Wallet>> getWallets() = 0;

   //! Register handler for load wallet messages. This callback is triggered by
   //! createWallet and loadWallet above, and also triggered when wallets are
   //! loaded at startup or by RPC.
   using LoadWalletFn = std::function<void(std::unique_ptr<Wallet> wallet)>;
   virtual std::unique_ptr<Handler> handleLoadWallet(LoadWalletFn fn) = 0;
};

//! Information about one wallet address.
struct WalletAddress
{
    CTxDestination dest;
    isminetype is_mine;
    std::string name;
    std::string purpose;

    WalletAddress(CTxDestination dest, isminetype is_mine, std::string name, std::string purpose)
        : dest(std::move(dest)), is_mine(is_mine), name(std::move(name)), purpose(std::move(purpose))
    {
    }
};

//! Collection of wallet balances.
struct WalletBalances
{
    CAmount balance = 0;
    CAmount stake = 0;
    CAmount unconfirmed_balance = 0;
    CAmount immature_balance = 0;
    bool have_watch_only = false;
    CAmount watch_only_balance = 0;
    CAmount unconfirmed_watch_only_balance = 0;
    CAmount immature_watch_only_balance = 0;
    //! What the HD accounts above 0 hold, trusted: not in balance, which is
    //! what account 0 can spend. Pending and immature are of the whole wallet.
    CAmount other_accounts = 0;

    bool balanceChanged(const WalletBalances& prev) const
    {
        return balance != prev.balance || stake != prev.stake || unconfirmed_balance != prev.unconfirmed_balance ||
               immature_balance != prev.immature_balance || watch_only_balance != prev.watch_only_balance ||
               unconfirmed_watch_only_balance != prev.unconfirmed_watch_only_balance ||
               immature_watch_only_balance != prev.immature_watch_only_balance || other_accounts != prev.other_accounts;
    }
};

//! An HD account as the GUI lists it.
struct WalletHDAccount
{
    uint32_t number = 0;
    std::string label;
    bool retired = false;
    CAmount trusted = 0;
    CAmount untrusted_pending = 0;
    CAmount immature = 0;
    //! Whether a transaction of the wallet pays it.
    bool used = false;
};

//! What the GUI shows and allows for the HD accounts of a wallet.
struct WalletHDAccountsState
{
    //! A legacy HD wallet with private keys: the only kind with accounts.
    bool supported = false;
    //! The wallet holds an account above 0.
    bool holds_accounts = false;
    //! Keys of accounts without the flag (an earlier wallet tool's salvage).
    bool inconsistent = false;
    //! The scan of a restored wallet is owed.
    bool scan_owed = false;
    //! The wallet is locked, and addresses of its accounts are still to be
    //! derived or found: after a payment seen while locked, a repair, or a
    //! scan that looks for accounts.
    bool unlock_owed = false;
    //! The wallet is looking for its accounts in the block chain.
    bool discovery_pending = false;
    //! Keys of a seed the wallet no longer has, which its phrase does not restore.
    int earlier_seed_keys = 0;
    //! Why New account would be refused without force; empty when it would not.
    bilingual_str new_account_refusal;
    //! The refusal is the account rule alone (the account before has no
    //! confirmed payment), which force overrides.
    bool new_account_forceable = false;
};

//! What a restore from a phrase found.
struct WalletRestoreReport
{
    //! bip32, bip39 or bip44.
    std::string hd_type;
    //! The path of the first receive address.
    std::string first_path;
    //! Receive and change addresses of account 0 found paid.
    int used_addresses = 0;
    //! The accounts above 0 found.
    std::vector<uint32_t> accounts_found;
    //! Whether the wallet holds any transaction.
    bool history = false;
    //! Whether the scan is still owed.
    bool scan_owed = false;
};

// Wallet transaction information.
struct WalletTx
{
    CTransactionRef tx;
    std::vector<isminetype> txin_is_mine;
    std::vector<isminetype> txout_is_mine;
    std::vector<CTxDestination> txout_address;
    std::vector<isminetype> txout_address_is_mine;
    //! The HD account of each input and output of the wallet; empty when the
    //! wallet holds no keys of an account above 0.
    std::vector<uint32_t> txin_account;
    std::vector<uint32_t> txout_account;
    CAmount credit;
    CAmount debit;
    CAmount change;
    int64_t time;
    std::map<std::string, std::string> value_map;
    bool is_coinbase;
    bool is_coinstake;
};

//! Updated transaction status.
struct WalletTxStatus
{
    int block_height;
    int blocks_to_maturity;
    int depth_in_main_chain;
    unsigned int time_received;
    uint32_t lock_time;
    bool is_final;
    bool is_trusted;
    bool is_abandoned;
    bool is_coinbase;
    bool is_coinstake;
    bool is_in_main_chain;
};

//! Wallet transaction output.
struct WalletTxOut
{
    CTxOut txout;
    int64_t time;
    int depth_in_main_chain = -1;
    bool is_spent = false;
};

//! Unspent wallet output as listed in the minting table.
struct WalletMintingOutput
{
    COutPoint outpoint;
    CTxOut txout;
    //! Timestamp of the transaction, which stake age is counted from.
    int64_t tx_time;
};

//! Return implementation of Wallet interface. This function is defined in
//! dummywallet.cpp and throws if the wallet component is not compiled.
std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet);

//! Return implementation of ChainClient interface for a wallet client. This
//! function will be undefined in builds where ENABLE_WALLET is false.
std::unique_ptr<WalletClient> MakeWalletClient(Chain& chain, ArgsManager& args);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_WALLET_H
