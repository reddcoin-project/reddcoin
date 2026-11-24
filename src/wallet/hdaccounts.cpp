// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// HD accounts of legacy wallets: creating an account, handing out its
// addresses, and what the wallet holds in each.

#include <chainparams.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <outputtype.h>
#include <util/check.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/receive.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

const std::string HD_ACCOUNTS_LEGACY_ONLY{"HD accounts are available in legacy wallets only. Use a separate wallet to keep funds apart."};

namespace {
//! The highest account above 0 handed out, 0 for none.
uint32_t HighestHandedOutHDAccount(const LegacyScriptPubKeyMan& spk_man)
{
    uint32_t highest{0};
    for (const auto& [number, record] : spk_man.GetHDAccounts()) {
        if (record.state != CHDAccount::State::POOL) highest = std::max(highest, number);
    }
    return highest;
}

//! BIP44's account rule, for an account `before` the next one with no confirmed payment.
bilingual_str HDAccountRule(uint32_t before)
{
    return strprintf(_("Account %u has not received a confirmed payment. A new account is created only after the one before it has been paid, so that the phrase of the wallet restores every account, in this wallet and in other BIP44 wallets. Pay account %u first, or pass force; an account created before that may not be found when the wallet is restored."), before, before);
}
} // namespace

bool CWallet::IsHDAccountUsed(uint32_t account, bool confirmed_only) const
{
    AssertLockHeld(cs_wallet);
    for (const auto& entry : mapWallet) {
        const auto& wtx = entry.second;
        if (wtx.isAbandoned()) continue;
        const int depth{wtx.GetDepthInMainChain()};
        if (depth < (confirmed_only ? 1 : 0)) continue;
        for (const CTxOut& txout : wtx.tx->vout) {
            if (IsMine(txout) != ISMINE_NO && GetHDAccountOfScript(txout.scriptPubKey) == account) return true;
        }
    }
    return false;
}

int64_t CWallet::HighestPaidHDAccountIndex(uint32_t account, bool internal) const
{
    AssertLockHeld(cs_wallet);
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    int64_t highest{-1};
    if (!spk_man) return highest;
    // A restore finds what is confirmed: an address counts as paid once a
    // payment to it is. (The change a payment takes has a rule of its own:
    // see GetHDAccountChangeDestination.)
    for (const auto& entry : mapWallet) {
        const auto& wtx = entry.second;
        if (wtx.isAbandoned() || wtx.GetDepthInMainChain() < 1) continue;
        for (const CTxOut& txout : wtx.tx->vout) {
            uint32_t key_account, index;
            bool key_internal;
            if (spk_man->GetHDAccountKeyOfScript(txout.scriptPubKey, key_account, key_internal, index) &&
                key_account == account && key_internal == internal) {
                highest = std::max(highest, int64_t{index});
            }
        }
    }
    return highest;
}

bool CWallet::GetNewHDAccountDestination(uint32_t account, bool internal, OutputType type, const std::string& label, bool force, CTxDestination& dest, std::vector<bilingual_str>& warnings, bilingual_str& error)
{
    AssertLockHeld(cs_wallet);
    LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    const std::map<uint32_t, CHDAccount> accounts{spk_man ? spk_man->GetHDAccounts() : std::map<uint32_t, CHDAccount>{}};
    const auto it = accounts.find(account);
    if (it == accounts.end() || it->second.state == CHDAccount::State::POOL) {
        error = strprintf(_("This wallet has no HD account %u."), account);
        return false;
    }
    if (it->second.state == CHDAccount::State::RETIRED) {
        error = strprintf(_("HD account %u is retired: it gives no new address. sethdaccount can bring it back."), account);
        return false;
    }
    if (type == OutputType::BECH32M) {
        error = _("Legacy wallets cannot provide bech32m addresses");
        return false;
    }
    const uint32_t index{internal ? it->second.next_change : it->second.next_receive};
    // The gap: a restore watches HD_ACCOUNT_KEY_LOOKAHEAD keys beyond the
    // last one paid, and an address beyond them may not be found.
    if (int64_t{index} > HighestPaidHDAccountIndex(account, internal) + HD_ACCOUNT_KEY_LOOKAHEAD) {
        const bilingual_str gap{_("The last 20 addresses of this account have not been paid. Use one of them (listed by listhdaccounts for the account), or pass force; an address beyond the gap may not be found when the wallet is restored.")};
        if (!force) {
            error = gap;
            return false;
        }
        warnings.push_back(gap);
    }
    CPubKey pubkey;
    if (!spk_man->GetHDAccountPubKey(account, internal, index, pubkey)) {
        error = _("Unlock the wallet to derive further addresses for this account.");
        return false;
    }
    dest = GetDestinationForKey(pubkey, type);
    if (!internal) SetAddressBook(dest, label, "receive");
    if (!spk_man->HandOutHDAccountKey(account, internal, index)) {
        error = strprintf(_("The address could not be written to the wallet for HD account %u."), account);
        return false;
    }
    return true;
}

bool CWallet::NewHDAccount(const std::string& label, bool force, uint32_t& account, CTxDestination& first, std::vector<bilingual_str>& warnings, bilingual_str& error)
{
    AssertLockHeld(cs_wallet);
    LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) || !spk_man) {
        error = Untranslated(HD_ACCOUNTS_LEGACY_ONLY);
        return false;
    }
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || !spk_man->IsHDEnabled()) {
        error = _("HD accounts need an HD wallet with private keys.");
        return false;
    }

    // The next account is the one after the highest handed out.
    const uint32_t highest{HighestHandedOutHDAccount(*spk_man)};
    if (highest + 1 >= 0x80000000) {
        error = _("This wallet has as many HD accounts as there can be.");
        return false;
    }
    account = highest + 1;
    // BIP44's account rule: the account before it has a confirmed payment.
    if (!IsHDAccountUsed(account - 1, /* confirmed_only */ true)) {
        const bilingual_str rule{HDAccountRule(account - 1)};
        if (!force) {
            error = rule;
            return false;
        }
        warnings.push_back(rule);
    }

    const bool first_account{!IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS)};
    if (first_account) {
        if (spk_man->IsHDAccountDiscoveryPending()) {
            error = _("This wallet is looking for its HD accounts in the block chain. Try again once that scan has finished.");
            return false;
        }
        if (IsLocked()) {
            error = _("Unlock the wallet to create its first HD account.");
            return false;
        }
        if (!spk_man->SetupHDAccounts()) {
            error = _("The HD accounts of this wallet could not be set up: its seed cannot be read.");
            return false;
        }
        warnings.push_back(_("This wallet now holds HD accounts. Versions without HD accounts refuse to open it, as requiring a newer version; the wallet is intact."));
        if (!IsCrypted()) {
            warnings.push_back(_("This wallet keeps its seed when it is encrypted: any copy of the wallet file made before that holds the seed unencrypted, and has to be destroyed. Make a new backup."));
            if (!spk_man->IsBip39Enabled()) {
                warnings.push_back(_("Encrypt this wallet before creating accounts if you intend to encrypt it at all."));
            }
        }
    }
    if (spk_man->GetHDAccountExtent(account).first < 0 && IsLocked()) {
        error = _("Unlock the wallet to derive further addresses for this account.");
        return false;
    }

    const std::map<uint32_t, CHDAccount> accounts{spk_man->GetHDAccounts()};
    CHDAccount record{accounts.count(account) ? accounts.at(account) : CHDAccount{}};
    record.state = CHDAccount::State::ACTIVE;
    record.label = label;
    record.nCreateTime = GetTime();
    if (!spk_man->UpdateHDAccount(account, record)) {
        error = strprintf(_("HD account %u could not be written to the wallet."), account);
        return false;
    }
    // The keys of an account created ahead of the rule, and the account kept ahead.
    spk_man->TopUpHDAccounts();
    OutputType type{m_default_address_type};
    if (type == OutputType::BECH32M) type = OutputType::BECH32;
    return GetNewHDAccountDestination(account, /* internal */ false, type, label, /* force */ false, first, warnings, error);
}

std::map<uint32_t, CWallet::Balance> CWallet::GetHDAccountBalances(int min_depth, bool avoid_reuse, bool other_accounts_only) const
{
    std::map<uint32_t, Balance> balances;
    if (!other_accounts_only) balances[0];
    if (const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan()) {
        for (const auto& entry : spk_man->GetHDAccounts()) balances[entry.first];
    }
    LOCK(cs_wallet);
    const bool skip_used{avoid_reuse && IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE)};
    std::set<uint256> trusted_parents;
    for (const auto& [txid, wtx] : mapWallet) {
        // Worked out for a transaction with an output counted, and only
        // then: a wallet's outputs are mostly of account 0.
        std::optional<bool> is_trusted;
        std::optional<int> depth;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i) {
            const CTxOut& txout = wtx.tx->vout[i];
            const uint32_t account{GetHDAccountOfScript(txout.scriptPubKey)};
            if (other_accounts_only && account == 0) continue;
            if ((IsMine(txout) & ISMINE_SPENDABLE) == ISMINE_NO || IsSpent(txid, i)) continue;
            if (skip_used && IsSpentKey(txid, i)) continue;
            if (!is_trusted) {
                is_trusted = IsTrusted(wtx, trusted_parents);
                depth = wtx.GetDepthInMainChain();
            }
            Balance& balance = balances[account];
            if (wtx.IsImmatureCoinBase()) {
                if (*depth > 0) balance.m_mine_immature += txout.nValue;
            } else if (*is_trusted && *depth >= min_depth) {
                balance.m_mine_trusted += txout.nValue;
            } else if (!*is_trusted && *depth == 0 && wtx.InMempool()) {
                balance.m_mine_untrusted_pending += txout.nValue;
            }
        }
    }
    return balances;
}

CAmount CWallet::GetOtherHDAccountsTrusted() const
{
    CAmount trusted{0};
    for (const auto& entry : GetHDAccountBalances(/* min_depth */ 0, /* avoid_reuse */ true, /* other_accounts_only */ true)) {
        const auto& balance = entry.second;
        trusted += balance.m_mine_trusted;
    }
    return trusted;
}

std::vector<CTxDestination> CWallet::GetUnpaidHDAccountAddresses(uint32_t account) const
{
    AssertLockHeld(cs_wallet);
    std::vector<CTxDestination> unpaid;
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) return unpaid;
    std::set<CScript> paid;
    for (const auto& entry : mapWallet) {
        const auto& wtx = entry.second;
        if (wtx.isAbandoned() || wtx.GetDepthInMainChain() < 0) continue;
        for (const CTxOut& txout : wtx.tx->vout) paid.insert(txout.scriptPubKey);
    }
    for (const auto& [dest, entry] : m_address_book) {
        if (entry.IsChange() || entry.purpose != "receive") continue;
        const CScript script{GetScriptForDestination(dest)};
        uint32_t key_account, index;
        bool internal;
        if (spk_man->GetHDAccountKeyOfScript(script, key_account, internal, index) && key_account == account && !internal && !paid.count(script)) {
            unpaid.push_back(dest);
        }
    }
    return unpaid;
}

bool CWallet::SetHDAccount(uint32_t account, const std::optional<std::string>& label, std::optional<bool> retired, bilingual_str& error)
{
    AssertLockHeld(cs_wallet);
    LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    const std::map<uint32_t, CHDAccount> accounts{spk_man ? spk_man->GetHDAccounts() : std::map<uint32_t, CHDAccount>{}};
    const auto it = accounts.find(account);
    if (it == accounts.end() || it->second.state == CHDAccount::State::POOL) {
        error = strprintf(_("This wallet has no HD account %u."), account);
        return false;
    }
    CHDAccount record{it->second};
    if (label) record.label = *label;
    if (retired) record.state = *retired ? CHDAccount::State::RETIRED : CHDAccount::State::ACTIVE;
    if (!spk_man->UpdateHDAccount(account, record)) {
        error = strprintf(_("HD account %u could not be written to the wallet."), account);
        return false;
    }
    // An account brought back gets its look-ahead again.
    spk_man->TopUpHDAccounts();
    return true;
}

CWallet::HDRestoreReport CWallet::GetHDRestoreReport() const
{
    HDRestoreReport report;
    LOCK(cs_wallet);
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) return report;
    report.first_path = spk_man->GetHDChain().IsBip44() ? strprintf("m/44'/%d'/0'/0/0", Params().ExtCoinType()) : "m/0'/0'/0'";
    report.history = !mapWallet.empty();
    std::set<CScript> used;
    for (const auto& entry : mapWallet) {
        const auto& wtx = entry.second;
        if (wtx.isAbandoned() || wtx.GetDepthInMainChain() < 0) continue;
        for (const CTxOut& txout : wtx.tx->vout) {
            if (IsMine(txout) != ISMINE_NO && GetHDAccountOfScript(txout.scriptPubKey) == 0) used.insert(txout.scriptPubKey);
        }
    }
    report.used_addresses = static_cast<int>(used.size());
    for (const auto& [account, record] : spk_man->GetHDAccounts()) {
        if (record.state == CHDAccount::State::ACTIVE) report.accounts_found.push_back(account);
    }
    return report;
}

bilingual_str CWallet::NewHDAccountRefusal() const
{
    AssertLockHeld(cs_wallet);
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) || !spk_man) return Untranslated(HD_ACCOUNTS_LEGACY_ONLY);
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || !spk_man->IsHDEnabled()) return _("HD accounts need an HD wallet with private keys.");
    const uint32_t highest{HighestHandedOutHDAccount(*spk_man)};
    if (highest + 1 >= 0x80000000) return _("This wallet has as many HD accounts as there can be.");
    if (!IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS) && spk_man->IsHDAccountDiscoveryPending()) {
        return _("This wallet is looking for its HD accounts in the block chain. Try again once that scan has finished.");
    }
    if (!IsHDAccountUsed(highest, /* confirmed_only */ true)) return HDAccountRule(highest);
    return {};
}

bilingual_str CWallet::HDAccountScanRefusal() const
{
    AssertLockHeld(cs_wallet);
    if (IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) return Untranslated(HD_ACCOUNTS_LEGACY_ONLY);
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) return _("This wallet has no private keys: it has no seed to derive accounts from");
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man || !spk_man->IsHDEnabled()) return _("This wallet is not an HD wallet: it has no seed to derive accounts from");
    if (!CanSupportFeature(FEATURE_HD_SPLIT)) return _("Cannot hold HD accounts in a wallet without HD chain split. Use the upgradewallet RPC first");
    // Accounts of the seed the wallet has now: of no use for the keys of the
    // one it had before.
    if (spk_man->HoldsInactiveSeedKeys() && !IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS)) {
        return _("The seed of this wallet was replaced after keys were made (by encryption or by sethdseed). Accounts of the earlier seed cannot be reached from this wallet. Restore the earlier seed into a new blank wallet.");
    }
    return {};
}

CWallet::HDAccountScan CWallet::ScanForHDAccounts(uint32_t count, const WalletRescanReserver& reserver)
{
    HDAccountScan scan;
    LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        scan.status = HDAccountScan::Status::CANNOT_DERIVE;
        return scan;
    }
    std::set<uint32_t> before;
    uint256 start_block;
    {
        LOCK(cs_wallet);
        if (!chain().hasBlocks(GetLastBlockHash(), 0, {})) {
            scan.status = HDAccountScan::Status::PRUNED;
            return scan;
        }
        CHECK_NONFATAL(chain().findAncestorByHeight(GetLastBlockHash(), 0, interfaces::FoundBlock().hash(start_block)));
        if (spk_man->IsHDAccountDiscoveryPending()) {
            scan.status = HDAccountScan::Status::BUSY;
            return scan;
        }
        for (const auto& [account, record] : spk_man->GetHDAccounts()) {
            if (record.state != CHDAccount::State::POOL) before.insert(account);
        }
        if (!spk_man->BeginHDAccountDiscovery(count)) {
            scan.status = HDAccountScan::Status::CANNOT_DERIVE;
            return scan;
        }
    }

    const ScanResult result{ScanForWalletTransactions(start_block, 0, /* max_height */ {}, reserver, /* fUpdate */ true)};
    {
        LOCK(cs_wallet);
        spk_man->EndHDAccountDiscovery();
        for (const auto& [account, record] : spk_man->GetHDAccounts()) {
            if (record.state == CHDAccount::State::POOL) continue;
            scan.accounts.push_back(account);
            if (!before.count(account)) scan.found.push_back(account);
        }
    }
    scan.stop_height = result.last_scanned_height;
    switch (result.status) {
    case ScanResult::SUCCESS:
        break;
    case ScanResult::FAILURE:
        scan.status = HDAccountScan::Status::FAILURE;
        break;
    case ScanResult::USER_ABORT:
        scan.status = HDAccountScan::Status::USER_ABORT;
        break;
    }
    return scan;
}

int CWallet::CountKeysOfEarlierSeed() const
{
    const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    return spk_man ? spk_man->CountInactiveSeedKeys() : 0;
}

bool CWallet::RepairHDAccounts()
{
    AssertLockHeld(cs_wallet);
    LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man || !spk_man->IsHDAccountRepairOwed()) return true;
    std::vector<uint32_t> found;
    if (!spk_man->RepairHDAccountKeys(found)) return false;
    // The payments the wallet holds to the accounts found move their next
    // indexes and keep the account after the highest used ahead.
    for (const auto& entry : mapWallet) {
        const auto& wtx = entry.second;
        if (wtx.isAbandoned() || wtx.GetDepthInMainChain() < 0) continue;
        for (const CTxOut& txout : wtx.tx->vout) {
            if (GetHDAccountOfScript(txout.scriptPubKey) != 0) spk_man->MarkUnusedAddresses(txout.scriptPubKey);
        }
    }
    return true;
}
