// Copyright (c) 2018-2020 The Bitcoin Core developers
// Copyright (c) 2018-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/wallet.h>

#include <amount.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <policy/fees.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/system.h>
#include <util/ui_change_type.h>
#include <wallet/context.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/ismine.h>
#include <wallet/load.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeHandler;
using interfaces::Wallet;
using interfaces::WalletAddress;
using interfaces::WalletBalances;
using interfaces::WalletClient;
using interfaces::WalletHDAccount;
using interfaces::WalletHDAccountsState;
using interfaces::WalletMintingOutput;
using interfaces::WalletOrderForm;
using interfaces::WalletRestoreReport;
using interfaces::WalletTx;
using interfaces::WalletTxOut;
using interfaces::WalletTxStatus;
using interfaces::WalletValueMap;

namespace wallet {
namespace {
//! Construct wallet tx struct.
WalletTx MakeWalletTx(CWallet& wallet, const CWalletTx& wtx)
{
    LOCK(wallet.cs_wallet);
    WalletTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(wallet.IsMine(txin));
    }
    result.txout_is_mine.reserve(wtx.tx->vout.size());
    result.txout_address.reserve(wtx.tx->vout.size());
    result.txout_address_is_mine.reserve(wtx.tx->vout.size());
    for (const auto& txout : wtx.tx->vout) {
        result.txout_is_mine.emplace_back(wallet.IsMine(txout));
        result.txout_address.emplace_back();
        result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                      wallet.IsMine(result.txout_address.back()) :
                                                      ISMINE_NO);
    }
    if (wallet.HoldsHDAccountKeys()) {
        result.txin_account.reserve(wtx.tx->vin.size());
        for (const auto& txin : wtx.tx->vin) {
            const auto prev = wallet.mapWallet.find(txin.prevout.hash);
            const bool known{prev != wallet.mapWallet.end() && txin.prevout.n < prev->second.tx->vout.size()};
            result.txin_account.push_back(known ? wallet.GetHDAccountOfScript(prev->second.tx->vout[txin.prevout.n].scriptPubKey) : 0);
        }
        result.txout_account.reserve(wtx.tx->vout.size());
        for (const auto& txout : wtx.tx->vout) result.txout_account.push_back(wallet.GetHDAccountOfScript(txout.scriptPubKey));
    }
    result.credit = wtx.GetCredit(ISMINE_ALL);
    result.debit = wtx.GetDebit(ISMINE_ALL);
    result.change = wtx.GetChange();
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    return result;
}

//! Construct wallet tx status struct.
WalletTxStatus MakeWalletTxStatus(const CWallet& wallet, const CWalletTx& wtx)
{
    WalletTxStatus result;
    result.block_height = wtx.m_confirm.block_height > 0 ? wtx.m_confirm.block_height : std::numeric_limits<int>::max();
    result.blocks_to_maturity = wtx.GetBlocksToMaturity();
    result.depth_in_main_chain = wtx.GetDepthInMainChain();
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    // Finality is settled for a confirmed transaction and for one the mempool
    // accepted; only an unconfirmed transaction the mempool does not hold, a
    // locktime transaction the wallet is keeping until it is final, needs the
    // chain asked, and that is the one case the GUI shows as "open until".
    result.is_final = result.depth_in_main_chain >= 1 || wtx.InMempool() || wallet.chain().checkFinalTx(*wtx.tx);
    result.is_trusted = wtx.IsTrusted();
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    result.is_in_main_chain = wtx.IsInMainChain();
    return result;
}

//! Construct wallet TxOut struct.
WalletTxOut MakeWalletTxOut(const CWallet& wallet,
    const CWalletTx& wtx,
    int n,
    int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    result.txout = wtx.tx->vout[n];
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(wtx.GetHash(), n);
    return result;
}

//! Append the outputs of a wallet transaction that the minting table lists.
void AppendMintingOutputs(const CWallet& wallet,
    const CWalletTx& wtx,
    std::vector<WalletMintingOutput>& result) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (wtx.GetDepthInMainChain() < (wtx.IsCoinBase() ? 2 : 1)) return;
    const uint256& hash = wtx.GetHash();
    for (unsigned int n = 0; n < wtx.tx->vout.size(); ++n) {
        const CTxOut& txout = wtx.tx->vout[n];
        if (wallet.IsSpent(hash, n) || !wallet.IsMine(txout)) continue;
        result.push_back({COutPoint(hash, n), txout, wtx.tx->nTime});
    }
}

class WalletImpl : public Wallet
{
public:
    explicit WalletImpl(const std::shared_ptr<CWallet>& wallet) : m_wallet(wallet) {}

    bool encryptWallet(const SecureString& wallet_passphrase) override
    {
        return m_wallet->EncryptWallet(wallet_passphrase);
    }
    bool isCrypted() override { return m_wallet->IsCrypted(); }
    bool lock() override { return m_wallet->Lock(); }
    bool unlock(const SecureString& wallet_passphrase) override { return m_wallet->Unlock(wallet_passphrase); }
    bool hdSecretsRewriteOnUnlock() override { return m_wallet->HDSecretsRewriteOnUnlock(); }
    std::vector<bilingual_str> takeHDSecretsMovedNotices() override { return m_wallet->TakeHDSecretsMovedNotices(); }
    std::vector<bilingual_str> getHDWarnings(bool with_advice) override { return m_wallet->GetHDWarnings(/* for_load */ with_advice); }
    bool hasKeysSeedDoesNotRestore() override { return m_wallet->HasKeysSeedDoesNotRestore(); }
    bool hdAccountsInconsistent() override
    {
        const LegacyScriptPubKeyMan* spk_man = m_wallet->GetLegacyScriptPubKeyMan();
        return spk_man && spk_man->HasHDAccountsInconsistency();
    }
    bool hdRestoreScanOwed() override { return m_wallet->IsHDRestoreScanOwed(); }
    bool hdAccountsUnlockOwed() override
    {
        const LegacyScriptPubKeyMan* spk_man = m_wallet->GetLegacyScriptPubKeyMan();
        return spk_man && m_wallet->IsLocked() && (spk_man->IsHDAccountsTopUpOwed() || spk_man->IsHDAccountRepairOwed());
    }
    WalletHDAccountsState getHDAccountsState() override
    {
        WalletHDAccountsState state;
        const LegacyScriptPubKeyMan* spk_man = m_wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) ? nullptr : m_wallet->GetLegacyScriptPubKeyMan();
        state.supported = spk_man && spk_man->IsHDEnabled() && !m_wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        if (!spk_man) return state;
        state.holds_accounts = m_wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS);
        state.inconsistent = spk_man->HasHDAccountsInconsistency();
        state.scan_owed = hdRestoreScanOwed();
        state.unlock_owed = hdAccountsUnlockOwed();
        state.discovery_pending = spk_man->IsHDAccountDiscoveryPending();
        state.earlier_seed_keys = m_wallet->CountKeysOfEarlierSeed();
        if (state.supported) {
            LOCK(m_wallet->cs_wallet);
            state.new_account_refusal = m_wallet->NewHDAccountRefusal(&state.new_account_forceable);
        }
        return state;
    }
    std::vector<WalletHDAccount> listHDAccounts(bool with_balances) override
    {
        std::vector<WalletHDAccount> result;
        const LegacyScriptPubKeyMan* spk_man = m_wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) ? nullptr : m_wallet->GetLegacyScriptPubKeyMan();
        const std::map<uint32_t, CWallet::Balance> balances{with_balances ? m_wallet->GetHDAccountBalances() : std::map<uint32_t, CWallet::Balance>{}};
        LOCK(m_wallet->cs_wallet);
        auto add = [&](uint32_t number, const CHDAccount* record) EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet) {
            WalletHDAccount& entry = result.emplace_back();
            entry.number = number;
            if (record) {
                entry.label = record->label;
                entry.retired = record->state == CHDAccount::State::RETIRED;
            }
            if (const auto it = balances.find(number); it != balances.end()) {
                entry.trusted = it->second.m_mine_trusted;
                entry.untrusted_pending = it->second.m_mine_untrusted_pending;
                entry.immature = it->second.m_mine_immature;
            }
            if (with_balances) entry.used = m_wallet->IsHDAccountUsed(number, /* confirmed_only */ false);
        };
        add(0, nullptr);
        if (spk_man) {
            for (const auto& [number, record] : spk_man->GetHDAccounts()) {
                if (record.state != CHDAccount::State::POOL) add(number, &record);
            }
        }
        return result;
    }
    bool newHDAccount(const std::string& label, bool force, uint32_t& account, CTxDestination& first,
        std::vector<bilingual_str>& warnings, bilingual_str& error) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->NewHDAccount(label, force, account, first, warnings, error);
    }
    bool setHDAccount(uint32_t account, const std::optional<std::string>& label, std::optional<bool> retired,
        bilingual_str& error) override
    {
        if (account == 0) {
            error = _("Account 0 is the wallet's own: it has no label or state of its own to change.");
            return false;
        }
        LOCK(m_wallet->cs_wallet);
        return m_wallet->SetHDAccount(account, label, retired, error);
    }
    std::vector<CTxDestination> getUnpaidHDAccountAddresses(uint32_t account) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetUnpaidHDAccountAddresses(account);
    }
    bool rescanHDAccounts(int count, std::vector<uint32_t>& found, bilingual_str& error) override
    {
        if (count < 1 || count > 20) {
            error = _("The number of accounts to look at must be from 1 to 20.");
            return false;
        }
        {
            LOCK(m_wallet->cs_wallet);
            error = m_wallet->HDAccountScanRefusal();
            if (!error.empty()) return false;
            if (m_wallet->IsLocked() || m_wallet->GetIsWalletStakingOnly()) {
                error = _("Unlock the wallet to look for its HD accounts.");
                return false;
            }
        }
        WalletRescanReserver reserver(*m_wallet);
        if (!reserver.reserve()) {
            error = _("The wallet is already scanning the block chain. Wait for that scan to finish.");
            return false;
        }
        const CWallet::HDAccountScan scan{m_wallet->ScanForHDAccounts(static_cast<uint32_t>(count), reserver)};
        found = scan.found;
        switch (scan.status) {
        case CWallet::HDAccountScan::Status::SUCCESS:
            return true;
        case CWallet::HDAccountScan::Status::BUSY:
            error = _("The wallet is already scanning the block chain. Wait for that scan to finish.");
            return false;
        case CWallet::HDAccountScan::Status::PRUNED:
            error = _("The blocks to scan have been pruned from this node.");
            return false;
        case CWallet::HDAccountScan::Status::CANNOT_DERIVE:
            error = _("The accounts of this wallet's seed cannot be derived now.");
            return false;
        case CWallet::HDAccountScan::Status::FAILURE:
            error = _("The scan failed: the block files may be damaged.");
            return false;
        case CWallet::HDAccountScan::Status::USER_ABORT:
            error = _("The scan was cancelled. The accounts found so far are kept.");
            return false;
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    }
    bool getNewHDAccountDestination(uint32_t account, OutputType type, const std::string& label,
        CTxDestination& dest, bilingual_str& error) override
    {
        LOCK(m_wallet->cs_wallet);
        if (account == 0) {
            std::string err;
            if (m_wallet->GetNewDestination(type, label, dest, err)) return true;
            error = Untranslated(err);
            return false;
        }
        std::vector<bilingual_str> warnings;
        return m_wallet->GetNewHDAccountDestination(account, /* internal */ false, type, label, /* force */ false, dest, warnings, error);
    }
    uint32_t getHDAccountOfScript(const CScript& script) override
    {
        return m_wallet->GetHDAccountOfScript(script);
    }
    WalletRestoreReport getRestoreReport() override
    {
        WalletRestoreReport report;
        const CWallet::HDRestoreReport restore{m_wallet->GetHDRestoreReport()};
        report.hd_type = m_wallet->IsBip44Enabled() ? "bip44" : m_wallet->IsBip39Enabled() ? "bip39" : "bip32";
        report.first_path = restore.first_path;
        report.used_addresses = restore.used_addresses;
        report.accounts_found = restore.accounts_found;
        report.history = restore.history;
        report.scan_owed = hdRestoreScanOwed();
        return report;
    }
    bool isLocked() override { return m_wallet->IsLocked(); }
    bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) override
    {
        return m_wallet->ChangeWalletPassphrase(old_wallet_passphrase, new_wallet_passphrase);
    }
    void abortRescan() override { m_wallet->AbortRescan(); }
    bool backupWallet(const std::string& filename) override { return m_wallet->BackupWallet(filename); }
    std::string getWalletName() override { return m_wallet->GetName(); }
    bool getNewDestination(const OutputType type, const std::string label, CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        std::string error;
        return m_wallet->GetNewDestination(type, label, dest, error);
    }
    bool getPubKey(const CScript& script, const CKeyID& address, CPubKey& pub_key) override
    {
        std::unique_ptr<SigningProvider> provider = m_wallet->GetSolvingProvider(script);
        if (provider) {
            return provider->GetPubKey(address, pub_key);
        }
        return false;
    }
    bool getStakeWeight(uint64_t& average_weight, uint64_t& total_weight) override
    {
        return m_wallet->GetPublishedStakeWeight(average_weight, total_weight);
    }
    void setEnableStaking(bool enableStaking) override
    {
        m_wallet->SetEnableStaking(enableStaking);
    }
    bool getEnableStaking() override
    {
        return m_wallet->GetEnableStaking();
    }
    void setUnlockWalletStaking(bool stakingOnly) override
    {
        m_wallet->SetIsWalletStakingOnly(stakingOnly);
    }
    bool getUnlockWalletStaking() override
    {
        return m_wallet->GetIsWalletStakingOnly();
    }
    int64_t getLastCoinStakeSearchInterval() override
    {
        return m_wallet->GetLastCoinStakeSearchInterval();
    }
    SigningResult signMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) override
    {
        return m_wallet->SignMessage(message, pkhash, str_sig);
    }
    bool isSpendable(const CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(dest) & ISMINE_SPENDABLE;
    }
    bool haveWatchOnly() override
    {
        auto spk_man = m_wallet->GetLegacyScriptPubKeyMan();
        if (spk_man) {
            return spk_man->HaveWatchOnly();
        }
        return false;
    };
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) override
    {
        return m_wallet->SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_wallet->DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) override
    {
        LOCK(m_wallet->cs_wallet);
        auto it = m_wallet->m_address_book.find(dest);
        if (it == m_wallet->m_address_book.end() || it->second.IsChange()) {
            return false;
        }
        if (name) {
            *name = it->second.GetLabel();
        }
        if (is_mine) {
            *is_mine = m_wallet->IsMine(dest);
        }
        if (purpose) {
            *purpose = it->second.purpose;
        }
        return true;
    }
    std::vector<WalletAddress> getAddresses() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletAddress> result;
        for (const auto& item : m_wallet->m_address_book) {
            if (item.second.IsChange()) continue;
            result.emplace_back(item.first, m_wallet->IsMine(item.first), item.second.GetLabel(), item.second.purpose);
        }
        return result;
    }
    std::vector<std::string> getAddressReceiveRequests() override {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetAddressReceiveRequests();
    }
    bool setAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& value) override {
        LOCK(m_wallet->cs_wallet);
        WalletBatch batch{m_wallet->GetDatabase()};
        return m_wallet->SetAddressReceiveRequest(batch, dest, id, value);
    }
    bool displayAddress(const CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->DisplayAddress(dest);
    }
    void lockCoin(const COutPoint& output) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->LockCoin(output);
    }
    void unlockCoin(const COutPoint& output) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->UnlockCoin(output);
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsLockedCoin(output.hash, output.n);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->ListLockedCoins(outputs);
    }
    CTransactionRef createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        bilingual_str& fail_reason) override
    {
        LOCK(m_wallet->cs_wallet);
        CTransactionRef tx;
        FeeCalculation fee_calc_out;
        if (!m_wallet->CreateTransaction(recipients, tx, fee, change_pos,
                fail_reason, coin_control, fee_calc_out, sign)) {
            return {};
        }
        return tx;
    }
    void commitTransaction(CTransactionRef tx,
        WalletValueMap value_map,
        WalletOrderForm order_form) override
    {
        LOCK(m_wallet->cs_wallet);
        m_wallet->CommitTransaction(std::move(tx), std::move(value_map), std::move(order_form));
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_wallet->TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AbandonTransaction(txid);
    }
    bool transactionCanBeBumped(const uint256& txid) override
    {
        return feebumper::TransactionCanBeBumped(*m_wallet.get(), txid);
    }
    bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        std::vector<bilingual_str>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) override
    {
        return feebumper::CreateRateBumpTransaction(*m_wallet.get(), txid, coin_control, errors, old_fee, new_fee, mtx) == feebumper::Result::OK;
    }
    bool signBumpTransaction(CMutableTransaction& mtx) override { return feebumper::SignTransaction(*m_wallet.get(), mtx); }
    bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<bilingual_str>& errors,
        uint256& bumped_txid) override
    {
        return feebumper::CommitTransaction(*m_wallet.get(), txid, std::move(mtx), errors, bumped_txid) ==
               feebumper::Result::OK;
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return mi->second.tx;
        }
        return {};
    }
    WalletTx getWalletTx(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return MakeWalletTx(*m_wallet, mi->second);
        }
        return {};
    }
    std::vector<WalletTx> getWalletTxs() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTx> result;
        result.reserve(m_wallet->mapWallet.size());
        for (const auto& entry : m_wallet->mapWallet) {
            result.emplace_back(MakeWalletTx(*m_wallet, entry.second));
        }
        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
        interfaces::WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& block_time) override
    {
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi == m_wallet->mapWallet.end()) {
            return false;
        }
        num_blocks = m_wallet->GetLastBlockHeight();
        block_time = -1;
        CHECK_NONFATAL(m_wallet->chain().findBlock(m_wallet->GetLastBlockHash(), FoundBlock().time(block_time)));
        tx_status = MakeWalletTxStatus(*m_wallet, mi->second);
        return true;
    }
    WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            num_blocks = m_wallet->GetLastBlockHeight();
            in_mempool = mi->second.InMempool();
            order_form = mi->second.vOrderForm;
            tx_status = MakeWalletTxStatus(*m_wallet, mi->second);
            return MakeWalletTx(*m_wallet, mi->second);
        }
        return {};
    }
    TransactionError fillPSBT(int sighash_type,
        bool sign,
        bool bip32derivs,
        size_t* n_signed,
        PartiallySignedTransaction& psbtx,
        bool& complete) override
    {
        return m_wallet->FillPSBT(psbtx, complete, sighash_type, sign, bip32derivs, n_signed);
    }
    WalletBalances getBalances() override
    {
        const auto bal = m_wallet->GetBalance();
        WalletBalances result;
        result.balance = bal.m_mine_trusted;
        result.stake = bal.m_mine_stake;
        result.unconfirmed_balance = bal.m_mine_untrusted_pending;
        result.immature_balance = bal.m_mine_immature;
        // Available is what account 0 can spend; the other HD accounts are
        // apart, and the payments of the GUI spend account 0.
        if (m_wallet->HoldsHDAccountKeys()) {
            result.other_accounts = m_wallet->GetOtherHDAccountsTrusted();
            result.balance -= result.other_accounts;
        }
        result.have_watch_only = haveWatchOnly();
        if (result.have_watch_only) {
            result.watch_only_balance = bal.m_watchonly_trusted;
            result.unconfirmed_watch_only_balance = bal.m_watchonly_untrusted_pending;
            result.immature_watch_only_balance = bal.m_watchonly_immature;
        }
        return result;
    }
    bool tryGetBalances(WalletBalances& balances, uint256& block_hash) override
    {
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        block_hash = m_wallet->GetLastBlockHash();
        balances = getBalances();
        return true;
    }
    CAmount getBalance() override { return m_wallet->GetBalance().m_mine_trusted; }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        return m_wallet->GetAvailableBalance(&coin_control);
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetCredit(txout, filter);
    }
    CoinsList listCoins(uint32_t account) override
    {
        LOCK(m_wallet->cs_wallet);
        CoinsList result;
        for (const auto& entry : m_wallet->ListCoins(account)) {
            auto& group = result[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(COutPoint(coin.tx->GetHash(), coin.i),
                    MakeWalletTxOut(*m_wallet, *coin.tx, coin.i, coin.nDepth));
            }
        }
        return result;
    }
    std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTxOut> result;
        result.reserve(outputs.size());
        for (const auto& output : outputs) {
            result.emplace_back();
            auto it = m_wallet->mapWallet.find(output.hash);
            if (it != m_wallet->mapWallet.end()) {
                int depth = it->second.GetDepthInMainChain();
                if (depth >= 0) {
                    result.back() = MakeWalletTxOut(*m_wallet, it->second, output.n, depth);
                }
            }
        }
        return result;
    }
    std::vector<WalletMintingOutput> getMintingOutputs() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletMintingOutput> result;
        for (const auto& entry : m_wallet->mapWallet) {
            AppendMintingOutputs(*m_wallet, entry.second, result);
        }
        return result;
    }
    std::vector<WalletMintingOutput> getMintingOutputs(const uint256& txid, std::vector<uint256>& txids) override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletMintingOutput> result;
        txids.assign(1, txid);
        auto it = m_wallet->mapWallet.find(txid);
        if (it == m_wallet->mapWallet.end()) return result;
        // A confirmed spend is announced under its own hash only, so the
        // transactions it spends from are answered for here as well.
        for (const CTxIn& txin : it->second.tx->vin) {
            if (std::find(txids.begin(), txids.end(), txin.prevout.hash) == txids.end()) {
                txids.push_back(txin.prevout.hash);
            }
        }
        for (const uint256& hash : txids) {
            auto mi = m_wallet->mapWallet.find(hash);
            if (mi != m_wallet->mapWallet.end()) AppendMintingOutputs(*m_wallet, mi->second, result);
        }
        return result;
    }
    CAmount getRequiredFee(unsigned int tx_bytes) override { return GetRequiredFee(*m_wallet, tx_bytes); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CAmount result;
        result = GetMinimumFee(*m_wallet, tx_bytes, coin_control, &fee_calc);
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
        return result;
    }
    unsigned int getConfirmTarget() override { return m_wallet->m_confirm_target; }
    bool hdEnabled() override { return m_wallet->IsHDEnabled(); }
    bool bip39Enabled() override { return m_wallet->IsBip39Enabled(); }
    bool bip44Enabled() override { return m_wallet->IsBip44Enabled(); }
    bool canGetAddresses() override { return m_wallet->CanGetAddresses(); }
    bool hasExternalSigner() override { return m_wallet->IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER); }
    bool privateKeysDisabled() override { return m_wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS); }
    OutputType getDefaultAddressType() override { return m_wallet->m_default_address_type; }
    CAmount getDefaultMaxTxFee() override { return m_wallet->m_default_max_tx_fee; }
    void remove() override
    {
        RemoveWallet(m_wallet, false /* load_on_start */);
    }
    bool isLegacy() override { return m_wallet->IsLegacy(); }
    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeHandler(m_wallet->NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(m_wallet->ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyStatusChanged.connect([fn](CWallet*) { fn(); }));
    }
    std::unique_ptr<Handler> handleNotifyWalletStakingStatusChanged(NotifyWalletStakingStatusChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyWalletStakingStatusChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyAddressBookChanged.connect(
            [fn](const CTxDestination& address, const std::string& label, bool is_mine,
                 const std::string& purpose, ChangeType status) { fn(address, label, is_mine, purpose, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTransactionChanged.connect(
            [fn](const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyWatchonlyChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyCanGetAddressesChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleHDAccountsChanged(HDAccountsChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyHDAccountsChanged.connect(fn));
    }
    CWallet* wallet() override { return m_wallet.get(); }

    std::shared_ptr<CWallet> m_wallet;
};

class WalletClientImpl : public WalletClient
{
public:
    WalletClientImpl(Chain& chain, ArgsManager& args)
    {
        m_context.chain = &chain;
        m_context.args = &args;
    }
    ~WalletClientImpl() override { UnloadWallets(); }

    //! ChainClient methods
    void registerRpcs() override
    {
        for (const CRPCCommand& command : GetWalletRPCCommands()) {
            m_rpc_commands.emplace_back(command.category, command.name, [this, &command](const JSONRPCRequest& request, UniValue& result, bool last_handler) {
                JSONRPCRequest wallet_request = request;
                wallet_request.context = &m_context;
                return command.actor(wallet_request, result, last_handler);
            }, command.argNames, command.unique_id);
            m_rpc_handlers.emplace_back(m_context.chain->handleRpc(m_rpc_commands.back()));
        }
    }
    bool verify() override { return VerifyWallets(*m_context.chain); }
    bool load() override { return LoadWallets(*m_context.chain); }
    void start(CScheduler& scheduler) override { return StartWallets(scheduler, *Assert(m_context.args)); }
    void flush() override { return FlushWallets(); }
    void stop() override { return StopWallets(); }
    void setMockTime(int64_t time) override { return SetMockTime(time); }

    //! WalletClient methods
    std::unique_ptr<Wallet> createWallet(const std::string& name, const SecureString& ssMnemonic, const SecureString& ssMnemonicPassphrase, const SecureString& ssMasterKey, const SecureString& passphrase, const int& walletType, const bool& importing, uint64_t wallet_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings) override
    {
        std::shared_ptr<CWallet> wallet;
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletOptions;
        options.require_create = true;
        options.create_flags = wallet_creation_flags;
        options.create_passphrase = passphrase;
        walletOptions.ssMnemonic = ssMnemonic;
        walletOptions.ssMnemonicPassphrase = ssMnemonicPassphrase;
        walletOptions.ssMasterKey = ssMasterKey;
        walletOptions.walletType = walletType;
        walletOptions.importing = importing;
        return MakeWallet(CreateWallet(*m_context.chain, name, true /* load_on_start */, options, walletOptions, status, error, warnings));
    }
    std::unique_ptr<Wallet> loadWallet(const std::string& name, bilingual_str& error, std::vector<bilingual_str>& warnings) override
    {
        DatabaseOptions options;
        DatabaseStatus status;
        options.require_existing = true;
        return MakeWallet(LoadWallet(*m_context.chain, name, true /* load_on_start */, options, status, error, warnings));
    }
    std::string getWalletDir() override
    {
        return GetWalletDir().string();
    }
    std::vector<std::string> listWalletDir() override
    {
        std::vector<std::string> paths;
        for (auto& path : ListDatabases(GetWalletDir())) {
            paths.push_back(path.string());
        }
        return paths;
    }
    std::vector<std::unique_ptr<Wallet>> getWallets() override
    {
        std::vector<std::unique_ptr<Wallet>> wallets;
        for (const auto& wallet : GetWallets()) {
            wallets.emplace_back(MakeWallet(wallet));
        }
        return wallets;
    }
    std::unique_ptr<Handler> handleLoadWallet(LoadWalletFn fn) override
    {
        return HandleLoadWallet(std::move(fn));
    }

    WalletContext m_context;
    const std::vector<std::string> m_wallet_filenames;
    std::vector<std::unique_ptr<Handler>> m_rpc_handlers;
    std::list<CRPCCommand> m_rpc_commands;
};
} // namespace
} // namespace wallet

namespace interfaces {
std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet) { return wallet ? std::make_unique<wallet::WalletImpl>(wallet) : nullptr; }

std::unique_ptr<WalletClient> MakeWalletClient(Chain& chain, ArgsManager& args)
{
    return std::make_unique<wallet::WalletClientImpl>(chain, args);
}
} // namespace interfaces
