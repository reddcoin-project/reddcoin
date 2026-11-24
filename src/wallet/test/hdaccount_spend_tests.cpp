// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <key_io.h>
#include <outputtype.h>
#include <policy/policy.h>
#include <pos/stake.h>
#include <script/sign.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/rbf.h>
#include <util/system.h>
#include <util/translation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
//! A bip39 wallet that holds HD accounts, on a chain whose coinbases pay
//! coinbaseKey, and a way to pay its scripts in a staked block.
struct HDAccountSpendSetup : public TestChain100Setup {
    std::shared_ptr<CWallet> wallet;

    HDAccountSpendSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
        // Proof of work ended at height 90: blocks from here are staked.
        stakeBlocks(1);
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        walletoptions.walletType = walletType::bip39Wallet;
        wallet = CreateWallet(*m_node.chain, "accounts", /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return spk_man.SetupHDAccounts()));
    }

    ~HDAccountSpendSetup()
    {
        SyncWithValidationInterfaceQueue();
        RemoveWallet(wallet, /* load_on_start */ std::nullopt);
        UnloadWallet(std::move(wallet));
    }

    static CMutableTransaction SimpleSpend(const CTransaction& from, uint32_t index, const CKey& key, const CScript& script)
    {
        CMutableTransaction mtx;
        mtx.vout.push_back({from.vout[index].nValue - DEFAULT_TRANSACTION_MAXFEE, script});
        mtx.vin.push_back({CTxIn{from.GetHash(), index}});
        FillableSigningProvider keystore;
        keystore.AddKey(key);
        std::map<COutPoint, Coin> coins;
        coins[mtx.vin[0].prevout].out = from.vout[index];
        std::map<int, std::string> input_errors;
        BOOST_REQUIRE(SignTransaction(mtx, &keystore, coins, SIGHASH_ALL, input_errors));
        return mtx;
    }

    //! The value of every coin Fund pays.
    std::optional<CAmount> m_coin_value;

    //! Pay each script a coinbase of the chain, in one staked block, and
    //! wait for the wallet to see it.
    void Fund(const std::vector<CScript>& scripts)
    {
        std::vector<CMutableTransaction> txs;
        {
            // Coins of coinbaseKey that nothing has spent, each locked so that
            // the block's own stake does not spend it too. All of one value,
            // which the tests count in: the fixture's coins differ in value
            // between the lines, and a stake changes a coin's value.
            LOCK(m_wallet->cs_wallet);
            std::vector<COutput> coins;
            m_wallet->AvailableCoins(coins);
            auto coin = coins.begin();
            for (const CScript& script : scripts) {
                while (coin != coins.end() && (!coin->fSpendable || coin->nDepth < 20 ||
                                               (m_coin_value && coin->tx->tx->vout[coin->i].nValue != *m_coin_value))) ++coin;
                BOOST_REQUIRE(coin != coins.end());
                m_coin_value = coin->tx->tx->vout[coin->i].nValue;
                m_wallet->LockCoin(COutPoint(coin->tx->GetHash(), coin->i));
                txs.push_back(SimpleSpend(*coin->tx->tx, coin->i, coinbaseKey, script));
                ++coin;
            }
        }
        SetMockTime(GetTime() + 60);
        CreateAndProcessPoSBlock(txs, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), m_wallet);
        wallet->BlockUntilSyncedToCurrentChain();
        LOCK(wallet->cs_wallet);
        for (const CMutableTransaction& tx : txs) {
            BOOST_REQUIRE(wallet->mapWallet.count(tx.GetHash()));
            BOOST_REQUIRE_EQUAL(wallet->mapWallet.at(tx.GetHash()).GetDepthInMainChain(), 1);
        }
    }

    CScript AccountZeroScript()
    {
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet->GetNewDestination(OutputType::BECH32, "", dest, error));
        return GetScriptForDestination(dest);
    }

    //! A script of key `index` of a chain of `account`, in the form given.
    CScript AccountScript(uint32_t account, bool internal, uint32_t index, OutputType type = OutputType::LEGACY)
    {
        CPubKey pubkey;
        BOOST_REQUIRE(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccountPubKey(account, internal, index, pubkey));
        return GetScriptForDestination(GetDestinationForKey(pubkey, type));
    }

    std::vector<COutput> Coins(const CCoinControl* coin_control)
    {
        std::vector<COutput> coins;
        WITH_LOCK(wallet->cs_wallet, wallet->AvailableCoins(coins, coin_control));
        return coins;
    }

    std::set<uint32_t> AccountsOf(const std::vector<COutput>& coins)
    {
        std::set<uint32_t> accounts;
        for (const COutput& out : coins) accounts.insert(wallet->GetHDAccountOfScript(out.tx->tx->vout[out.i].scriptPubKey));
        return accounts;
    }

    //! The accounts of the inputs of a transaction of the wallet.
    std::set<uint32_t> InputAccounts(const CTransaction& tx)
    {
        LOCK(wallet->cs_wallet);
        std::set<uint32_t> accounts;
        for (const CTxIn& in : tx.vin) {
            const CWalletTx& prev = wallet->mapWallet.at(in.prevout.hash);
            accounts.insert(wallet->GetHDAccountOfScript(prev.tx->vout[in.prevout.n].scriptPubKey));
        }
        return accounts;
    }

    struct Funded {
        CTransactionRef tx;
        int change_pos{-1};
        bilingual_str error;
        bool ok{false};
    };
    Funded Pay(const CScript& to, CAmount amount, const CCoinControl& coin_control)
    {
        Funded funded;
        CAmount fee;
        FeeCalculation fee_calc;
        funded.ok = wallet->CreateTransaction({CRecipient{to, amount, false}}, funded.tx, fee, funded.change_pos, funded.error, coin_control, fee_calc);
        return funded;
    }

    //! The phrase of the fixture's wallet, which is not encrypted.
    SecureString Phrase()
    {
        const CHDChain chain{Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDChain()};
        return SecureString(chain.vchMnemonic.begin(), chain.vchMnemonic.end());
    }

    //! Restore the fixture wallet's phrase into a new wallet, as createwallet does.
    std::shared_ptr<CWallet> Restore(const std::string& name, bool hd_accounts, const SecureString& passphrase = {})
    {
        DatabaseOptions options;
        options.require_create = true;
        options.create_passphrase = passphrase;
        WalletOptions walletoptions;
        walletoptions.walletType = walletType::bip39Wallet;
        walletoptions.importing = true;
        walletoptions.ssMnemonic = Phrase();
        walletoptions.hd_accounts = hd_accounts;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        std::shared_ptr<CWallet> restored = CreateWallet(*m_node.chain, name, std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(restored, error.original);
        return restored;
    }

    std::shared_ptr<CWallet> Reload(std::shared_ptr<CWallet>&& restored)
    {
        const std::string name{restored->GetName()};
        SyncWithValidationInterfaceQueue();
        RemoveWallet(restored, std::nullopt);
        UnloadWallet(std::move(restored));
        DatabaseOptions options;
        options.require_existing = true;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        std::shared_ptr<CWallet> loaded = LoadWallet(*m_node.chain, name, std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(loaded, error.original);
        return loaded;
    }

    static void Release(std::shared_ptr<CWallet>&& restored)
    {
        SyncWithValidationInterfaceQueue();
        RemoveWallet(restored, std::nullopt);
        UnloadWallet(std::move(restored));
    }

    static size_t AccountCoins(CWallet& restored, uint32_t account)
    {
        CCoinControl every_account;
        every_account.m_hd_all_accounts = true;
        std::vector<COutput> coins;
        WITH_LOCK(restored.cs_wallet, restored.AvailableCoins(coins, &every_account));
        size_t count{0};
        for (const COutput& out : coins) count += restored.GetHDAccountOfScript(out.tx->tx->vout[out.i].scriptPubKey) == account;
        return count;
    }

    static bool ScanOwed(CWallet& restored)
    {
        int height;
        return WITH_LOCK(restored.cs_wallet, return WalletBatch(restored.GetDatabase()).ReadRestoreScan(height));
    }

    static CScript Elsewhere()
    {
        CKey key;
        key.MakeNewKey(true);
        return GetScriptForDestination(PKHash(key.GetPubKey()));
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdaccount_spend_tests, HDAccountSpendSetup)

// A payment that names no account spends account 0, one that names an
// account spends that account's coins, and the change of each goes back to
// the account it was paid from. Listings and balances follow the account;
// the staker and listunspent see every account.
BOOST_AUTO_TEST_CASE(payment_spends_one_account)
{
    Fund({AccountZeroScript(), AccountZeroScript(), AccountScript(1, false, 0), AccountScript(1, false, 1, OutputType::BECH32)});
    CCoinControl account_one;
    account_one.m_hd_account = 1;
    CCoinControl every_account;
    every_account.m_hd_all_accounts = true;

    BOOST_CHECK_EQUAL(Coins(nullptr).size(), 2U);
    BOOST_CHECK(AccountsOf(Coins(nullptr)) == std::set<uint32_t>{0});
    BOOST_CHECK_EQUAL(Coins(&account_one).size(), 2U);
    BOOST_CHECK(AccountsOf(Coins(&account_one)) == std::set<uint32_t>{1});
    BOOST_CHECK_EQUAL(Coins(&every_account).size(), 4U);
    const CAmount coin{Coins(nullptr).front().tx->tx->vout[Coins(nullptr).front().i].nValue};
    BOOST_CHECK_EQUAL(wallet->GetAvailableBalance(), 2 * coin);
    BOOST_CHECK_EQUAL(wallet->GetAvailableBalance(&account_one), 2 * coin);
    // What the GUI shows apart, counted without looking at account 0's outputs.
    BOOST_CHECK_EQUAL(wallet->GetOtherHDAccountsTrusted(), 2 * coin);
    BOOST_CHECK_EQUAL(wallet->GetHDAccountBalances().at(1).m_mine_trusted, 2 * coin);
    BOOST_CHECK_EQUAL(wallet->GetHDAccountBalances(0, true, /* other_accounts_only */ true).count(0), 0U);
    {
        LOCK(wallet->cs_wallet);
        size_t listed{0};
        for (const auto& entry : wallet->ListCoins()) {
            const auto& coins = entry.second;
            for (const COutput& out : coins) {
                BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(out.tx->tx->vout[out.i].scriptPubKey), 0U);
                ++listed;
            }
        }
        BOOST_CHECK_EQUAL(listed, 2U);
        listed = 0;
        for (const auto& entry : wallet->ListCoins(1)) listed += entry.second.size();
        BOOST_CHECK_EQUAL(listed, 2U);
    }
    // The staker stakes the coins of every account.
    StakeCandidates candidates;
    BOOST_REQUIRE(CollectStakeCandidates(wallet.get(), candidates));
    BOOST_CHECK_EQUAL(candidates.coins.size(), 4U);

    // Account 0, change to account 0.
    const Funded zero{Pay(Elsewhere(), coin + coin / 2, CCoinControl{})};
    BOOST_REQUIRE_MESSAGE(zero.ok, zero.error.original);
    BOOST_CHECK(InputAccounts(*zero.tx) == std::set<uint32_t>{0});
    BOOST_REQUIRE_GE(zero.change_pos, 0);
    BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(zero.tx->vout[zero.change_pos].scriptPubKey), 0U);

    // Account 1, change to account 1's change chain.
    const Funded one{Pay(Elsewhere(), coin + coin / 2, account_one)};
    BOOST_REQUIRE_MESSAGE(one.ok, one.error.original);
    BOOST_CHECK(InputAccounts(*one.tx) == std::set<uint32_t>{1});
    BOOST_REQUIRE_GE(one.change_pos, 0);
    const CScript change{one.tx->vout[one.change_pos].scriptPubKey};
    BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(change), 1U);
    // The first change key of account 1, in the form the change takes.
    BOOST_CHECK(change == AccountScript(1, true, 0, OutputType::LEGACY) || change == AccountScript(1, true, 0, OutputType::P2SH_SEGWIT) ||
                change == AccountScript(1, true, 0, OutputType::BECH32));
    BOOST_CHECK_EQUAL(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccounts().at(1).next_change, 1U);

    // More than account 1 holds, though not more than the wallet: refused,
    // with what the other accounts hold.
    const Funded too_much{Pay(Elsewhere(), 3 * coin, account_one)};
    BOOST_CHECK(!too_much.ok);
    BOOST_CHECK_MESSAGE(too_much.error.original.find("Insufficient funds in HD account 1") != std::string::npos, too_much.error.original);
    BOOST_CHECK_MESSAGE(too_much.error.original.find("in other HD accounts") != std::string::npos, too_much.error.original);
    // The same with only the inputs given, as send and walletcreatefundedpsbt
    // fund by default.
    CCoinControl given_only{account_one};
    given_only.m_add_inputs = false;
    for (const COutput& out : Coins(&account_one)) given_only.Select(COutPoint(out.tx->GetHash(), out.i));
    const Funded too_much_given{Pay(Elsewhere(), 3 * coin, given_only)};
    BOOST_CHECK(!too_much_given.ok);
    BOOST_CHECK_MESSAGE(too_much_given.error.original.find("in other HD accounts") != std::string::npos, too_much_given.error.original);

    // An account that does not exist.
    CCoinControl account_nine;
    account_nine.m_hd_account = 9;
    const Funded nine{Pay(Elsewhere(), coin / 2, account_nine)};
    BOOST_CHECK(!nine.ok);
}

// Inputs picked by hand decide the account when none is named, and are
// refused when they are of two accounts, or of another account than the one
// named. A change address given is refused when it is not of the account
// spent.
BOOST_AUTO_TEST_CASE(selected_inputs_and_change)
{
    Fund({AccountZeroScript(), AccountScript(1, false, 0), AccountScript(1, false, 2)});
    CCoinControl every_account;
    every_account.m_hd_all_accounts = true;
    COutPoint zero_coin;
    std::vector<COutPoint> one_coins;
    CAmount coin{0};
    for (const COutput& out : Coins(&every_account)) {
        coin = out.tx->tx->vout[out.i].nValue;
        if (wallet->GetHDAccountOfScript(out.tx->tx->vout[out.i].scriptPubKey) == 0) {
            zero_coin = COutPoint(out.tx->GetHash(), out.i);
        } else {
            one_coins.emplace_back(out.tx->GetHash(), out.i);
        }
    }
    BOOST_REQUIRE_EQUAL(one_coins.size(), 2U);

    // The account of the inputs.
    CCoinControl inferred;
    inferred.Select(one_coins[0]);
    inferred.fAllowOtherInputs = true;
    const Funded from_inputs{Pay(Elsewhere(), coin + coin / 2, inferred)};
    BOOST_REQUIRE_MESSAGE(from_inputs.ok, from_inputs.error.original);
    BOOST_CHECK(InputAccounts(*from_inputs.tx) == std::set<uint32_t>{1});
    BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(from_inputs.tx->vout[from_inputs.change_pos].scriptPubKey), 1U);

    // Inputs of two accounts.
    CCoinControl mixed;
    mixed.Select(zero_coin);
    mixed.Select(one_coins[1]);
    const Funded two{Pay(Elsewhere(), coin / 2, mixed)};
    BOOST_CHECK(!two.ok);
    BOOST_CHECK_MESSAGE(two.error.original.find("of HD accounts 0, 1") != std::string::npos, two.error.original);

    // An input of another account than the one named.
    CCoinControl named;
    named.m_hd_account = 0;
    named.Select(one_coins[1]);
    const Funded other{Pay(Elsewhere(), coin / 2, named)};
    BOOST_CHECK(!other.ok);
    BOOST_CHECK_MESSAGE(other.error.original.find("A selected input is of HD account 1") != std::string::npos, other.error.original);

    // Change to account 0 from account 1, and to an address of no account
    // of the wallet: refused. To an address elsewhere from account 0: as
    // before accounts.
    CCoinControl wrong_change;
    wrong_change.m_hd_account = 1;
    CTxDestination dest;
    std::string error;
    BOOST_REQUIRE(wallet->GetNewDestination(OutputType::LEGACY, "", dest, error));
    wrong_change.destChange = dest;
    BOOST_CHECK(!Pay(Elsewhere(), coin / 2, wrong_change).ok);
    CTxDestination elsewhere_dest;
    BOOST_REQUIRE(ExtractDestination(Elsewhere(), elsewhere_dest));
    wrong_change.destChange = elsewhere_dest;
    BOOST_CHECK(!Pay(Elsewhere(), coin / 2, wrong_change).ok);
    CCoinControl zero_elsewhere;
    zero_elsewhere.destChange = elsewhere_dest;
    const Funded allowed{Pay(Elsewhere(), coin / 2, zero_elsewhere)};
    BOOST_CHECK_MESSAGE(allowed.ok, allowed.error.original);
}

// The receive gap counts confirmed payments only, as a restore finds them: an
// unconfirmed payment to the last of 20 addresses handed out does not let a
// 21st be handed out.
BOOST_AUTO_TEST_CASE(receive_gap_counts_confirmed_payments)
{
    uint32_t account;
    CTxDestination dest;
    std::vector<bilingual_str> warnings;
    bilingual_str error;
    LOCK(wallet->cs_wallet);
    BOOST_REQUIRE_MESSAGE(wallet->NewHDAccount("", /* force */ true, account, dest, warnings, error), error.original);
    BOOST_REQUIRE_EQUAL(account, 1U);
    for (uint32_t index = 1; index < HD_ACCOUNT_KEY_LOOKAHEAD; ++index) {
        BOOST_REQUIRE_MESSAGE(wallet->GetNewHDAccountDestination(1, /* internal */ false, OutputType::LEGACY, "", /* force */ false, dest, warnings, error), error.original);
    }
    CMutableTransaction unconfirmed;
    unconfirmed.vin.emplace_back(COutPoint(uint256::ONE, 0));
    unconfirmed.vout.emplace_back(COIN, GetScriptForDestination(dest));
    wallet->AddToWallet(MakeTransactionRef(unconfirmed), CWalletTx::Confirmation{});
    BOOST_CHECK(!wallet->GetNewHDAccountDestination(1, /* internal */ false, OutputType::LEGACY, "", /* force */ false, dest, warnings, error));
    BOOST_CHECK_MESSAGE(error.original.find("have not been paid") != std::string::npos, error.original);
}

// Every funding call of an account takes a change index, sent or not. When
// the last 20 handed out are all unpaid, the lowest of them is handed out
// again, so that a restore, which watches 20, still finds change.
BOOST_AUTO_TEST_CASE(change_gap)
{
    std::vector<CTxDestination> handed_out;
    LOCK(wallet->cs_wallet);
    for (uint32_t i = 0; i < HD_ACCOUNT_KEY_LOOKAHEAD; ++i) {
        CTxDestination dest;
        bilingual_str error;
        BOOST_REQUIRE_MESSAGE(wallet->GetHDAccountChangeDestination(1, OutputType::BECH32, dest, error), error.original);
        handed_out.push_back(dest);
    }
    BOOST_CHECK_EQUAL(std::set<CTxDestination>(handed_out.begin(), handed_out.end()).size(), size_t{HD_ACCOUNT_KEY_LOOKAHEAD});
    CTxDestination again;
    bilingual_str error;
    BOOST_REQUIRE(wallet->GetHDAccountChangeDestination(1, OutputType::BECH32, again, error));
    BOOST_CHECK(again == handed_out.front());
    BOOST_CHECK_EQUAL(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccounts().at(1).next_change, HD_ACCOUNT_KEY_LOOKAHEAD);
}

BOOST_AUTO_TEST_CASE(change_gap_moves_on_once_change_is_paid)
{
    CTxDestination first;
    {
        LOCK(wallet->cs_wallet);
        for (uint32_t i = 0; i < HD_ACCOUNT_KEY_LOOKAHEAD; ++i) {
            CTxDestination dest;
            bilingual_str error;
            BOOST_REQUIRE(wallet->GetHDAccountChangeDestination(1, OutputType::BECH32, dest, error));
            if (i == 5) first = dest;
        }
    }
    Fund({GetScriptForDestination(first)});
    LOCK(wallet->cs_wallet);
    CTxDestination next;
    bilingual_str error;
    BOOST_REQUIRE(wallet->GetHDAccountChangeDestination(1, OutputType::BECH32, next, error));
    BOOST_CHECK(next == GetDestinationForKey([&] {
        CPubKey pubkey;
        BOOST_REQUIRE(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccountPubKey(1, true, HD_ACCOUNT_KEY_LOOKAHEAD, pubkey));
        return pubkey;
    }(), OutputType::BECH32));
}

// A fee bump spends the account of the original inputs and keeps its change.
// A transaction whose change is of another account than its inputs, as one
// made before accounts could be, is not bumped.
BOOST_AUTO_TEST_CASE(fee_bump_stays_in_its_account)
{
    Fund({AccountScript(1, false, 0), AccountScript(1, false, 1)});
    CCoinControl account_one;
    account_one.m_hd_account = 1;
    account_one.m_signal_bip125_rbf = true;
    CCoinControl every_account;
    every_account.m_hd_all_accounts = true;
    const CAmount coin{Coins(&every_account).front().tx->tx->vout[Coins(&every_account).front().i].nValue};

    const Funded paid{Pay(Elsewhere(), coin / 2, account_one)};
    BOOST_REQUIRE_MESSAGE(paid.ok, paid.error.original);
    wallet->CommitTransaction(paid.tx, {}, {});
    std::vector<bilingual_str> errors;
    CAmount old_fee, new_fee;
    CMutableTransaction bumped;
    BOOST_CHECK(feebumper::CreateRateBumpTransaction(*wallet, paid.tx->GetHash(), CCoinControl{}, errors, old_fee, new_fee, bumped) == feebumper::Result::OK);
    BOOST_CHECK(InputAccounts(CTransaction(bumped)) == std::set<uint32_t>{1});
    bool kept_change{false};
    for (const CTxOut& out : bumped.vout) kept_change |= out.scriptPubKey == paid.tx->vout[paid.change_pos].scriptPubKey;
    BOOST_CHECK(kept_change);

    // Inputs of account 1, change to account 0, signed and sent as such.
    CMutableTransaction crossed;
    COutPoint input;
    for (const COutput& out : Coins(&account_one)) {
        if (out.tx->GetHash() != paid.tx->GetHash()) input = COutPoint(out.tx->GetHash(), out.i);
    }
    BOOST_REQUIRE(!input.IsNull());
    crossed.vin.emplace_back(input, CScript(), MAX_BIP125_RBF_SEQUENCE);
    crossed.vout.emplace_back(coin / 4, Elsewhere());
    CTxDestination zero_change;
    std::string change_error;
    BOOST_REQUIRE(wallet->GetNewChangeDestination(OutputType::LEGACY, zero_change, change_error));
    crossed.vout.emplace_back(coin - coin / 4 - coin / 100, GetScriptForDestination(zero_change));
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->SignTransaction(crossed)));
    const CTransactionRef crossed_tx{MakeTransactionRef(crossed)};
    wallet->CommitTransaction(crossed_tx, {}, {});
    errors.clear();
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->mapWallet.count(crossed_tx->GetHash())));
    BOOST_CHECK(feebumper::CreateRateBumpTransaction(*wallet, crossed_tx->GetHash(), CCoinControl{}, errors, old_fee, new_fee, bumped) != feebumper::Result::OK);
    BOOST_REQUIRE(!errors.empty());
    BOOST_CHECK_MESSAGE(errors.front().original.find("change address is not of HD account 1") != std::string::npos, errors.front().original);
}

// A restore whose phrase has no account used: the account kept ahead in
// memory for the scan is forgotten, and nothing of an account is left.
BOOST_AUTO_TEST_CASE(restore_without_accounts_keeps_nothing_of_them)
{
    Fund({AccountZeroScript()});
    std::shared_ptr<CWallet> restored = Restore("nothing", /* hd_accounts */ true);
    LegacyScriptPubKeyMan& spk_man = *Assert(restored->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK(!spk_man.IsHDAccountDiscoveryPending());
    BOOST_CHECK(!spk_man.HasHDAccountKeys());
    BOOST_CHECK(spk_man.GetHDAccounts().empty());
    BOOST_CHECK_EQUAL(restored->GetVersion(), FEATURE_LATEST);
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 0), 1U);
    Release(std::move(restored));
}

// A restore cut short leaves its scan owed, and the scan at the next load
// looks for the accounts again. One that had not looked is made here with
// hd_accounts false and the record of the scan owed written back.
BOOST_AUTO_TEST_CASE(restore_scan_owed_finds_accounts_at_the_next_load)
{
    Fund({AccountScript(1, false, 0), AccountScript(1, false, 1, OutputType::BECH32)});
    std::shared_ptr<CWallet> restored = Restore("cut_short", /* hd_accounts */ false);
    BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 0U);
    BOOST_REQUIRE(WITH_LOCK(restored->cs_wallet, return WalletBatch(restored->GetDatabase()).WriteRestoreScan(0)));

    restored = Reload(std::move(restored));
    BOOST_CHECK(restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(restored->GetVersion(), FEATURE_HD_ACCOUNTS);
    BOOST_CHECK(Assert(restored->GetLegacyScriptPubKeyMan())->GetHDAccounts().at(1).state == CHDAccount::State::ACTIVE);
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 2U);
    BOOST_CHECK(!ScanOwed(*restored));
    Release(std::move(restored));
}

// An encrypted wallet whose restore was cut short cannot derive at load, so
// its scan stays owed until it is unlocked; then the wallet's worker scans,
// and finds the accounts.
BOOST_AUTO_TEST_CASE(encrypted_restore_scan_owed_finds_accounts_at_unlock)
{
    const SecureString passphrase{"passphrase"};
    Fund({AccountScript(1, false, 0)});
    std::shared_ptr<CWallet> restored = Restore("cut_short_encrypted", /* hd_accounts */ false, passphrase);
    BOOST_REQUIRE(restored->IsLocked());
    BOOST_REQUIRE(WITH_LOCK(restored->cs_wallet, return WalletBatch(restored->GetDatabase()).WriteRestoreScan(0)));

    restored = Reload(std::move(restored));
    BOOST_REQUIRE(restored->IsLocked());
    BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK(ScanOwed(*restored));

    BOOST_REQUIRE(restored->Unlock(passphrase));
    restored->SyncWithHDAccountWorker();
    BOOST_CHECK(restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 1U);
    BOOST_CHECK(!ScanOwed(*restored));
    BOOST_REQUIRE(restored->Lock());
    Release(std::move(restored));
}

BOOST_AUTO_TEST_SUITE_END()
