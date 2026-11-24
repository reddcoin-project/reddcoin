// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// HD accounts of legacy wallets: creating an account, handing out its
// addresses, and what the wallet holds in each.

#include <key_io.h>
#include <outputtype.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/receive.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

const std::string HD_ACCOUNTS_LEGACY_ONLY{"HD accounts are available in legacy wallets only. Use a separate wallet to keep funds apart."};

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
    uint32_t highest{0};
    for (const auto& [number, record] : spk_man->GetHDAccounts()) {
        if (record.state != CHDAccount::State::POOL) highest = std::max(highest, number);
    }
    if (highest + 1 >= 0x80000000) {
        error = _("This wallet has as many HD accounts as there can be.");
        return false;
    }
    account = highest + 1;
    // BIP44's account rule: the account before it has a confirmed payment.
    if (!IsHDAccountUsed(account - 1, /* confirmed_only */ true)) {
        const bilingual_str rule{strprintf(_("Account %u has not received a confirmed payment. A new account is created only after the one before it has been paid, so that the phrase of the wallet restores every account, in this wallet and in other BIP44 wallets. Pay account %u first, or pass force; an account created before that may not be found when the wallet is restored."), account - 1, account - 1)};
        if (!force) {
            error = rule;
            return false;
        }
        warnings.push_back(rule);
    }

    const bool first_account{!IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS)};
    if (first_account) {
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

std::map<uint32_t, CWallet::Balance> CWallet::GetHDAccountBalances(int min_depth, bool avoid_reuse) const
{
    std::map<uint32_t, Balance> balances;
    balances[0];
    if (const LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan()) {
        for (const auto& entry : spk_man->GetHDAccounts()) balances[entry.first];
    }
    LOCK(cs_wallet);
    const bool skip_used{avoid_reuse && IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE)};
    std::set<uint256> trusted_parents;
    for (const auto& [txid, wtx] : mapWallet) {
        const bool is_trusted{IsTrusted(wtx, trusted_parents)};
        const int depth{wtx.GetDepthInMainChain()};
        const bool immature{wtx.IsImmatureCoinBase()};
        for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i) {
            const CTxOut& txout = wtx.tx->vout[i];
            if ((IsMine(txout) & ISMINE_SPENDABLE) == ISMINE_NO || IsSpent(txid, i)) continue;
            if (skip_used && IsSpentKey(txid, i)) continue;
            Balance& balance = balances[GetHDAccountOfScript(txout.scriptPubKey)];
            if (immature) {
                if (depth > 0) balance.m_mine_immature += txout.nValue;
            } else if (is_trusted && depth >= min_depth) {
                balance.m_mine_trusted += txout.nValue;
            } else if (!is_trusted && depth == 0 && wtx.InMempool()) {
                balance.m_mine_untrusted_pending += txout.nValue;
            }
        }
    }
    return balances;
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
