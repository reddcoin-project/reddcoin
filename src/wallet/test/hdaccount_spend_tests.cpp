// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <outputtype.h>
#include <policy/policy.h>
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
#include <wallet/staking.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
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
    //! wait for the wallet to see it, unless it is not to: `seen` false.
    void Fund(const std::vector<CScript>& scripts, bool seen = true)
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
        if (!seen) return;
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

    //! The bytes of a record's key, written as they are.
    struct RawKey {
        std::vector<unsigned char> bytes;
        template <typename Stream>
        void Serialize(Stream& s) const { s.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    };

    //! What the wallet tool of a version before HD accounts makes of a
    //! wallet's file when it salvages it: the keys, the master keys, the
    //! chain and its secrets, and a flags record holding the bit that says
    //! the secrets are encrypted. The metadata and witness scripts of the
    //! keys, the version, the other flags, the account records and the
    //! transactions are gone. The wallet is loaded again from the result:
    //! without a chain if `attach` is false, so that its worker does not run.
    std::shared_ptr<CWallet> OldSalvage(std::shared_ptr<CWallet>&& salvaged, bool attach = true)
    {
        const std::string name{salvaged->GetName()};
        Release(std::move(salvaged));
        {
            DatabaseOptions options;
            options.require_existing = true;
            DatabaseStatus status;
            bilingual_str error;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
            BOOST_REQUIRE_MESSAGE(database, error.original);
            std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
            uint64_t flags{0};
            BOOST_REQUIRE(batch->Read(DBKeys::FLAGS, flags));
            const std::set<std::string> kept{DBKeys::KEY, "wkey", DBKeys::MASTER_KEY, DBKeys::CRYPTED_KEY, DBKeys::HDCHAIN, DBKeys::HDSECRETS};
            std::vector<RawKey> erased;
            bool secrets{false};
            BOOST_REQUIRE(batch->StartCursor());
            while (true) {
                CDataStream key(SER_DISK, CLIENT_VERSION);
                CDataStream value(SER_DISK, CLIENT_VERSION);
                bool complete;
                const bool read{batch->ReadAtCursor(key, value, complete)};
                if (complete) break;
                BOOST_REQUIRE(read);
                RawKey raw{std::vector<unsigned char>(key.begin(), key.end())};
                std::string type;
                key >> type;
                secrets |= type == DBKeys::HDSECRETS;
                if (!kept.count(type)) erased.push_back(std::move(raw));
            }
            batch->CloseCursor();
            for (const RawKey& raw : erased) BOOST_REQUIRE(batch->Erase(raw));
            if (secrets) BOOST_REQUIRE(batch->Write(DBKeys::FLAGS, flags & WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        }
        if (!attach) {
            DatabaseOptions options;
            options.require_existing = true;
            DatabaseStatus status;
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
            BOOST_REQUIRE_MESSAGE(database, error.original);
            std::shared_ptr<CWallet> loaded = CWallet::Create(/* chain */ nullptr, name, std::move(database), 0, WalletOptions{}, error, warnings);
            BOOST_REQUIRE_MESSAGE(loaded, error.original);
            return loaded;
        }
        DatabaseOptions options;
        options.require_existing = true;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        std::shared_ptr<CWallet> loaded = LoadWallet(*m_node.chain, name, std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(loaded, error.original);
        return loaded;
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
// hd_accounts false and the records of the scan owed written back as a
// restore that looks leaves them.
BOOST_AUTO_TEST_CASE(restore_scan_owed_finds_accounts_at_the_next_load)
{
    Fund({AccountScript(1, false, 0), AccountScript(1, false, 1, OutputType::BECH32)});
    std::shared_ptr<CWallet> restored = Restore("cut_short", /* hd_accounts */ false);
    BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 0U);
    BOOST_REQUIRE(WITH_LOCK(restored->cs_wallet, return WalletBatch(restored->GetDatabase()).EraseRestoreScan() && WalletBatch(restored->GetDatabase()).WriteRestoreScan(0)));

    restored = Reload(std::move(restored));
    BOOST_CHECK(restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(restored->GetVersion(), FEATURE_HD_ACCOUNTS);
    BOOST_CHECK(Assert(restored->GetLegacyScriptPubKeyMan())->GetHDAccounts().at(1).state == CHDAccount::State::ACTIVE);
    BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 2U);
    BOOST_CHECK(!ScanOwed(*restored));
    Release(std::move(restored));
}

// A restore asked not to look for HD accounts and cut short does not look at
// the next load either, nor at the unlock of an encrypted wallet: the choice
// is in the file until the scan has completed.
BOOST_AUTO_TEST_CASE(declined_restore_does_not_look_when_done_again)
{
    const SecureString passphrase{"passphrase"};
    Fund({AccountScript(1, false, 0)});
    for (const bool encrypted : {false, true}) {
        std::shared_ptr<CWallet> restored = Restore(strprintf("declined_%d", encrypted), /* hd_accounts */ false, encrypted ? passphrase : SecureString{});
        // Cut short: the scan is owed again, and the choice is still in the file.
        BOOST_REQUIRE(WITH_LOCK(restored->cs_wallet, return WalletBatch(restored->GetDatabase()).WriteRestoreScan(0) && WalletBatch(restored->GetDatabase()).WriteHDAccountsDeclined()));
        restored = Reload(std::move(restored));
        if (encrypted) {
            BOOST_REQUIRE(restored->Unlock(passphrase));
            restored->SyncWithHDAccountWorker();
        }
        BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
        BOOST_CHECK_EQUAL(restored->GetVersion(), FEATURE_LATEST);
        BOOST_CHECK_EQUAL(AccountCoins(*restored, 1), 0U);
        BOOST_CHECK(!ScanOwed(*restored));
        if (encrypted) BOOST_REQUIRE(restored->Lock());
        Release(std::move(restored));
    }
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
    BOOST_REQUIRE(WITH_LOCK(restored->cs_wallet, return WalletBatch(restored->GetDatabase()).EraseRestoreScan() && WalletBatch(restored->GetDatabase()).WriteRestoreScan(0)));

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

// An account that a scan holds in memory only and that is made the wallet's
// during the scan, here handed out with force while rescanhdaccounts looks
// two accounts ahead, keeps every key when the scan ends: the keys held in
// memory are written before those derived after them.
BOOST_AUTO_TEST_CASE(account_made_during_a_scan_keeps_its_keys)
{
    {
        LOCK(wallet->cs_wallet);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_REQUIRE(spk_man.BeginHDAccountDiscovery(/* count */ 3));
        uint32_t account;
        CTxDestination dest;
        std::vector<bilingual_str> warnings;
        bilingual_str error;
        for (const uint32_t expected : {1U, 2U}) {
            BOOST_REQUIRE_MESSAGE(wallet->NewHDAccount("", /* force */ true, account, dest, warnings, error), error.original);
            BOOST_REQUIRE_EQUAL(account, expected);
        }
        spk_man.EndHDAccountDiscovery();
    }
    for (int load = 0; load < 2; ++load) {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDAccounts().at(2).state == CHDAccount::State::ACTIVE);
        BOOST_CHECK(spk_man.GetHDAccountExtent(2) == std::make_pair(int64_t{HD_ACCOUNT_KEY_LOOKAHEAD}, int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
        for (const bool internal : {false, true}) {
            CPubKey pubkey;
            BOOST_CHECK(spk_man.GetHDAccountPubKey(2, internal, 0, pubkey));
            BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(GetScriptForDestination(PKHash(pubkey))), 2U);
        }
        // Account 3 was looked at only.
        BOOST_CHECK(spk_man.GetHDAccountExtent(3) == std::make_pair(int64_t{-1}, int64_t{-1}));
        if (load == 0) wallet = Reload(std::move(wallet));
    }
}

// The keys kept ahead are written one by one: a crash during a refill leaves
// fewer. The wallet refills them when it is loaded.
BOOST_AUTO_TEST_CASE(short_lookahead_is_refilled_at_load)
{
    LegacyScriptPubKeyMan* spk_man = Assert(wallet->GetLegacyScriptPubKeyMan());
    WITH_LOCK(wallet->cs_wallet, spk_man->MarkUnusedAddresses(AccountScript(1, false, 5)));
    BOOST_REQUIRE_EQUAL(spk_man->GetHDAccountExtent(1).first, int64_t{5 + HD_ACCOUNT_KEY_LOOKAHEAD});
    std::vector<CPubKey> lost;
    for (uint32_t index = HD_ACCOUNT_KEY_LOOKAHEAD + 1; index <= 5 + HD_ACCOUNT_KEY_LOOKAHEAD; ++index) {
        CPubKey pubkey;
        BOOST_REQUIRE(spk_man->GetHDAccountPubKey(1, false, index, pubkey));
        lost.push_back(pubkey);
    }
    const std::string name{wallet->GetName()};
    Release(std::move(wallet));
    {
        DatabaseOptions options;
        options.require_existing = true;
        DatabaseStatus status;
        bilingual_str error;
        std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
        BOOST_REQUIRE_MESSAGE(database, error.original);
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        for (const CPubKey& pubkey : lost) {
            const CScript witprog{GetScriptForDestination(WitnessV0KeyHash(pubkey.GetID()))};
            BOOST_REQUIRE(batch->Erase(std::make_pair(DBKeys::KEY, pubkey)));
            BOOST_REQUIRE(batch->Erase(std::make_pair(DBKeys::KEYMETA, pubkey)));
            BOOST_REQUIRE(batch->Erase(std::make_pair(DBKeys::CSCRIPT, uint160(Hash160(witprog)))));
        }
    }
    DatabaseOptions options;
    options.require_existing = true;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    wallet = LoadWallet(*m_node.chain, name, std::nullopt, options, status, error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    wallet->SyncWithHDAccountWorker();
    spk_man = Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_CHECK_EQUAL(spk_man->GetHDAccountExtent(1).first, int64_t{5 + HD_ACCOUNT_KEY_LOOKAHEAD});
    for (const CPubKey& pubkey : lost) {
        BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->IsMine(GetScriptForDestination(PKHash(pubkey)))));
    }
}

// A scan that finds a payment to the last key derived ahead of an account
// while the wallet is locked, as when an unlock with a timeout ends during a
// long scan, cannot derive the keys after it: the scan from that block stays
// owed, and the worker does it at the next unlock, which finds a payment to a
// key beyond those the wallet had.
BOOST_AUTO_TEST_CASE(scan_owed_survives_a_relock)
{
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    const CScript last_ahead{AccountScript(1, false, HD_ACCOUNT_KEY_LOOKAHEAD - 1)};
    CKey key;
    CKeyMetadata meta;
    BOOST_REQUIRE(WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(1, false, HD_ACCOUNT_KEY_LOOKAHEAD + 5, key, meta)));
    const CScript beyond{GetScriptForDestination(PKHash(key.GetPubKey()))};
    const SecureString passphrase{"passphrase"};
    BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
    wallet->Lock();
    BOOST_REQUIRE(wallet->IsLocked());
    // A scan is owed from the start of the chain, as a restore leaves it.
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return WalletBatch(wallet->GetDatabase()).WriteRestoreScan(0)));
    Fund({last_ahead});
    const int due_height{WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHeight())};
    Fund({beyond}, /* seen */ false);
    BOOST_REQUIRE_EQUAL(AccountCoins(*wallet, 1), 1U);

    // The scan, run while locked.
    BOOST_REQUIRE(wallet->ScanOwedRestore());
    int owed_height{-1};
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return WalletBatch(wallet->GetDatabase()).ReadRestoreScan(owed_height)));
    BOOST_CHECK_EQUAL(owed_height, due_height);

    BOOST_REQUIRE(wallet->Unlock(passphrase));
    wallet->SyncWithHDAccountWorker();
    BOOST_CHECK_EQUAL(AccountCoins(*wallet, 1), 2U);
    BOOST_CHECK(!ScanOwed(*wallet));
    BOOST_REQUIRE(wallet->Lock());
}

// A plain HD (bip32) wallet whose keys of HD accounts an earlier wallet tool's
// salvage left unmarked keeps its seed when it is encrypted before the repair
// has run, as a marked one does: with a new seed, its account keys would be
// keys of account 0 for good.
BOOST_AUTO_TEST_CASE(unmarked_account_keys_keep_the_seed)
{
    DatabaseOptions options;
    options.require_create = true;
    WalletOptions walletoptions;
    walletoptions.walletType = walletType::bip32Wallet;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    std::shared_ptr<CWallet> plain = CreateWallet(*m_node.chain, "plain_accounts", std::nullopt, options, walletoptions, status, error, warnings);
    BOOST_REQUIRE_MESSAGE(plain, error.original);
    LegacyScriptPubKeyMan* spk_man = Assert(plain->GetLegacyScriptPubKeyMan());
    BOOST_REQUIRE(WITH_LOCK(plain->cs_wallet, return spk_man->SetupHDAccounts()));
    const auto key_of = [&](uint32_t account) {
        CKey key;
        CKeyMetadata meta;
        BOOST_REQUIRE(WITH_LOCK(spk_man->cs_KeyStore, return spk_man->DeriveHDAccountKey(account, false, 0, key, meta)));
        return key.GetPubKey();
    };
    const CPubKey zero_before{key_of(0)};
    const CPubKey one_before{key_of(1)};

    plain = OldSalvage(std::move(plain), /* attach */ false);
    spk_man = Assert(plain->GetLegacyScriptPubKeyMan());
    BOOST_REQUIRE(!plain->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_REQUIRE(spk_man->IsHDAccountRepairOwed());
    BOOST_CHECK(spk_man->KeepsHDSeed());
    const SecureString passphrase{"passphrase"};
    BOOST_REQUIRE(plain->EncryptWallet(passphrase));
    BOOST_REQUIRE(plain->Unlock(passphrase));
    BOOST_CHECK(key_of(0) == zero_before);
    BOOST_CHECK(key_of(1) == one_before);
    plain->SyncWithHDAccountWorker();
    BOOST_CHECK(plain->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(plain->GetHDAccountOfScript(GetScriptForDestination(PKHash(one_before))), 1U);
}

// A scan looking for HD accounts holds keys in memory that are not the
// wallet's: the wallet is not encrypted meanwhile, which would write them,
// and its first account is not created, which the scan does if it finds one.
BOOST_AUTO_TEST_CASE(no_encryption_or_first_account_during_a_scan)
{
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return spk_man.BeginHDAccountDiscovery(/* count */ 2)));
    BOOST_REQUIRE(spk_man.IsHDAccountDiscoveryPending());
    BOOST_CHECK(!wallet->EncryptWallet("passphrase"));
    BOOST_CHECK(!wallet->IsCrypted());
    WITH_LOCK(wallet->cs_wallet, spk_man.EndHDAccountDiscovery());
    BOOST_CHECK(spk_man.GetHDAccountExtent(2) == std::make_pair(int64_t{-1}, int64_t{-1}));

    std::shared_ptr<CWallet> restored = Restore("first_during_scan", /* hd_accounts */ false);
    LegacyScriptPubKeyMan& restored_spk_man = *Assert(restored->GetLegacyScriptPubKeyMan());
    {
        LOCK(restored->cs_wallet);
        BOOST_REQUIRE(restored_spk_man.BeginHDAccountDiscovery(/* count */ 1));
        uint32_t account;
        CTxDestination dest;
        std::vector<bilingual_str> warnings;
        bilingual_str error;
        BOOST_CHECK(!restored->NewHDAccount("", /* force */ true, account, dest, warnings, error));
        BOOST_CHECK_MESSAGE(error.original.find("looking for its HD accounts") != std::string::npos, error.original);
        restored_spk_man.EndHDAccountDiscovery();
    }
    BOOST_CHECK(!restored->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    Release(std::move(restored));
}

// A wallet with HD accounts salvaged by the wallet tool of an earlier version
// has its account keys without their metadata, and would spend them as
// account 0's. Once the seed can be read, the wallet's worker matches them
// against the seed's accounts: it writes their metadata back, the account
// records, the flag and the version, and notes the payments the wallet holds
// to them, as when they were received.
BOOST_AUTO_TEST_CASE(repair_after_an_old_salvage)
{
    for (const bool encrypted : {false, true}) {
        const SecureString passphrase{"passphrase"};
        if (encrypted) {
            // A fresh wallet for the second pass: the first one is salvaged.
            DatabaseOptions options;
            options.require_create = true;
            options.create_passphrase = passphrase;
            WalletOptions walletoptions;
            walletoptions.walletType = walletType::bip39Wallet;
            DatabaseStatus status;
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            Release(std::move(wallet));
            wallet = CreateWallet(*m_node.chain, "accounts_encrypted", std::nullopt, options, walletoptions, status, error, warnings);
            BOOST_REQUIRE_MESSAGE(wallet, error.original);
            BOOST_REQUIRE(wallet->Unlock(passphrase));
            BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return Assert(wallet->GetLegacyScriptPubKeyMan())->SetupHDAccounts()));
        }
        Fund({AccountZeroScript(), AccountScript(1, false, 2), AccountScript(1, false, 3, OutputType::BECH32)});
        Fund({AccountScript(2, true, 0, OutputType::P2SH_SEGWIT)});
        BOOST_REQUIRE_EQUAL(AccountCoins(*wallet, 1), 2U);
        BOOST_REQUIRE_EQUAL(AccountCoins(*wallet, 2), 1U);
        const std::map<uint32_t, CHDAccount> accounts_before{Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccounts()};
        if (encrypted) BOOST_REQUIRE(wallet->Lock());

        wallet = OldSalvage(std::move(wallet));
        LegacyScriptPubKeyMan* spk_man = Assert(wallet->GetLegacyScriptPubKeyMan());
        if (encrypted) {
            // Locked: the account keys are taken for account 0's until the
            // unlock. The scan at the load found every coin: a witness script
            // is learned with its key. (A plain wallet's worker may have
            // repaired it already: the load hands it the repair at once.)
            BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
            BOOST_CHECK(spk_man->IsHDAccountRepairOwed());
            wallet->SyncWithHDAccountWorker();
            BOOST_CHECK(spk_man->IsHDAccountRepairOwed());
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 1), 0U);
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 2), 0U);
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 0), 4U);
            BOOST_REQUIRE(wallet->Unlock(passphrase));
        }
        wallet->SyncWithHDAccountWorker();
        for (int load = 0; load < 2; ++load) {
            spk_man = Assert(wallet->GetLegacyScriptPubKeyMan());
            BOOST_CHECK(!spk_man->IsHDAccountRepairOwed());
            BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
            BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 0), 1U);
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 1), 2U);
            BOOST_CHECK_EQUAL(AccountCoins(*wallet, 2), 1U);
            const std::map<uint32_t, CHDAccount> accounts{spk_man->GetHDAccounts()};
            BOOST_REQUIRE_EQUAL(accounts.size(), accounts_before.size());
            for (const auto& [account, record] : accounts_before) {
                BOOST_CHECK(accounts.at(account).state == record.state);
                BOOST_CHECK_EQUAL(accounts.at(account).next_receive, record.next_receive);
                BOOST_CHECK_EQUAL(accounts.at(account).next_change, record.next_change);
            }
            BOOST_CHECK_EQUAL(spk_man->GetHDAccountMeta()->highest_used, 2U);
            BOOST_CHECK(!ScanOwed(*wallet));
            if (load == 0) wallet = Reload(std::move(wallet));
            if (encrypted && load == 0) {
                BOOST_REQUIRE(wallet->IsLocked());
                // Nothing is owed: the repair was written.
                BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->IsHDAccountRepairOwed());
            }
        }
    }
}

// The repair matches the seed's accounts, not any key: a salvaged wallet
// that never had an account is left unmarked, and is not asked again.
BOOST_AUTO_TEST_CASE(repair_without_accounts_leaves_the_wallet_unmarked)
{
    DatabaseOptions options;
    options.require_create = true;
    WalletOptions walletoptions;
    walletoptions.walletType = walletType::bip39Wallet;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    std::shared_ptr<CWallet> plain = CreateWallet(*m_node.chain, "plain", std::nullopt, options, walletoptions, status, error, warnings);
    BOOST_REQUIRE_MESSAGE(plain, error.original);
    CTxDestination dest;
    std::string dest_error;
    BOOST_REQUIRE(plain->GetNewDestination(OutputType::LEGACY, "", dest, dest_error));
    plain = OldSalvage(std::move(plain));
    plain->SyncWithHDAccountWorker();
    LegacyScriptPubKeyMan& spk_man = *Assert(plain->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(!spk_man.IsHDAccountRepairOwed());
    BOOST_CHECK(!plain->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK(spk_man.GetHDAccounts().empty());
    BOOST_CHECK(!spk_man.HasHDAccountKeys());
    BOOST_CHECK(WITH_LOCK(plain->cs_wallet, return plain->IsMine(dest)));
    // Its keys were given metadata: nothing is owed at the next load.
    plain = Reload(std::move(plain));
    BOOST_CHECK(!Assert(plain->GetLegacyScriptPubKeyMan())->IsHDAccountRepairOwed());
    BOOST_CHECK(!plain->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_LT(plain->GetVersion(), int{FEATURE_HD_ACCOUNTS});
    BOOST_CHECK(WITH_LOCK(plain->cs_wallet, return plain->IsMine(dest)));
    Release(std::move(plain));
}

// What the GUI reads and does through interfaces::Wallet: the accounts handed
// out, with their balances when asked; whether New account would be refused,
// and why; the coins of one account; the account of each input and output of
// a transaction; renaming, retiring, addresses; the scan for accounts; and
// the restore report.
BOOST_AUTO_TEST_CASE(gui_interface)
{
    std::unique_ptr<interfaces::Wallet> gui{interfaces::MakeWallet(wallet)};
    // Account 0 alone is handed out: the account kept ahead is not listed.
    BOOST_REQUIRE_EQUAL(gui->listHDAccounts(/* with_balances */ false).size(), 1U);
    interfaces::WalletHDAccountsState state{gui->getHDAccountsState()};
    BOOST_CHECK(state.supported);
    BOOST_CHECK(state.holds_accounts);
    BOOST_CHECK(!state.unlock_owed);
    BOOST_CHECK_EQUAL(state.earlier_seed_keys, 0);
    // Account 0 has no confirmed payment: New account would be refused.
    BOOST_CHECK_MESSAGE(state.new_account_refusal.original.rfind("Account 0 has not received a confirmed payment", 0) == 0, state.new_account_refusal.original);
    // That is the account rule alone, which force overrides.
    BOOST_CHECK(state.new_account_forceable);

    uint32_t account{0};
    CTxDestination first;
    std::vector<bilingual_str> warnings;
    bilingual_str error;
    BOOST_REQUIRE_MESSAGE(gui->newHDAccount("savings", /* force */ true, account, first, warnings, error), error.original);
    BOOST_CHECK_EQUAL(account, 1U);
    // Account 1 has no payment either.
    BOOST_CHECK(gui->getHDAccountsState().new_account_refusal.original.rfind("Account 1 has not received", 0) == 0);
    Fund({AccountZeroScript(), GetScriptForDestination(first)});

    std::vector<interfaces::WalletHDAccount> accounts{gui->listHDAccounts(/* with_balances */ true)};
    BOOST_REQUIRE_EQUAL(accounts.size(), 2U);
    BOOST_CHECK_EQUAL(accounts[0].number, 0U);
    BOOST_CHECK_EQUAL(accounts[0].trusted, wallet->GetAvailableBalance());
    BOOST_CHECK(accounts[0].used);
    BOOST_CHECK_EQUAL(accounts[1].number, 1U);
    BOOST_CHECK_EQUAL(accounts[1].label, "savings");
    BOOST_CHECK(!accounts[1].retired);
    BOOST_CHECK(accounts[1].used);
    BOOST_CHECK_GT(accounts[1].trusted, 0);
    // Without balances: numbers, labels and states only.
    BOOST_CHECK(!gui->listHDAccounts(false)[1].used);
    BOOST_CHECK_EQUAL(gui->listHDAccounts(false)[1].trusted, 0);
    // Account 1 is paid: the next can be created.
    BOOST_CHECK(gui->getHDAccountsState().new_account_refusal.empty());
    BOOST_CHECK(!gui->getHDAccountsState().new_account_forceable);

    // The coins of one account.
    size_t listed{0};
    for (const auto& entry : gui->listCoins(1)) {
        for (const auto& coin : entry.second) {
            BOOST_CHECK_EQUAL(gui->getHDAccountOfScript(std::get<1>(coin).txout.scriptPubKey), 1U);
            ++listed;
        }
    }
    BOOST_CHECK_EQUAL(listed, 1U);
    listed = 0;
    for (const auto& entry : gui->listCoins(0)) listed += entry.second.size();
    BOOST_CHECK_EQUAL(listed, 1U);

    // A payment of account 1: its input and its change are account 1's, and
    // a script of no wallet is of account 0.
    CCoinControl account_one;
    account_one.m_hd_account = 1;
    const Funded paid{Pay(Elsewhere(), accounts[1].trusted / 2, account_one)};
    BOOST_REQUIRE_MESSAGE(paid.ok, paid.error.original);
    BOOST_REQUIRE_GE(paid.change_pos, 0);
    wallet->CommitTransaction(paid.tx, {}, {});
    const interfaces::WalletTx wtx{gui->getWalletTx(paid.tx->GetHash())};
    BOOST_REQUIRE_EQUAL(wtx.txin_account.size(), paid.tx->vin.size());
    BOOST_REQUIRE_EQUAL(wtx.txout_account.size(), paid.tx->vout.size());
    for (const uint32_t input : wtx.txin_account) BOOST_CHECK_EQUAL(input, 1U);
    for (size_t i = 0; i < paid.tx->vout.size(); ++i) {
        BOOST_CHECK_EQUAL(wtx.txout_account[i], int(i) == paid.change_pos ? 1U : 0U);
    }

    // Rename, retire, bring back; account 0 has nothing of its own to change.
    BOOST_CHECK(gui->setHDAccount(1, std::string{"rainy day"}, std::nullopt, error));
    BOOST_CHECK(gui->setHDAccount(1, std::nullopt, true, error));
    accounts = gui->listHDAccounts(false);
    BOOST_CHECK_EQUAL(accounts[1].label, "rainy day");
    BOOST_CHECK(accounts[1].retired);
    BOOST_CHECK(!gui->setHDAccount(0, std::string{"mine"}, std::nullopt, error));
    BOOST_CHECK(!gui->setHDAccount(7, std::string{"none"}, std::nullopt, error));
    // A retired account gives no new address.
    CTxDestination dest;
    BOOST_CHECK(!gui->getNewHDAccountDestination(1, OutputType::LEGACY, "", dest, error));
    BOOST_CHECK(error.original.find("retired") != std::string::npos);
    BOOST_CHECK(gui->setHDAccount(1, std::nullopt, false, error));
    BOOST_REQUIRE_MESSAGE(gui->getNewHDAccountDestination(1, OutputType::LEGACY, "rent", dest, error), error.original);
    BOOST_CHECK_EQUAL(gui->getHDAccountOfScript(GetScriptForDestination(dest)), 1U);
    const std::vector<CTxDestination> unpaid{gui->getUnpaidHDAccountAddresses(1)};
    BOOST_CHECK(std::find(unpaid.begin(), unpaid.end(), dest) != unpaid.end());
    BOOST_CHECK(std::find(unpaid.begin(), unpaid.end(), first) == unpaid.end());
    // Account 0 through the same call.
    BOOST_REQUIRE_MESSAGE(gui->getNewHDAccountDestination(0, OutputType::BECH32, "", dest, error), error.original);
    BOOST_CHECK_EQUAL(gui->getHDAccountOfScript(GetScriptForDestination(dest)), 0U);

    // The scan for accounts: a count out of range is refused; on this wallet
    // it finds nothing it did not hold.
    std::vector<uint32_t> found;
    BOOST_CHECK(!gui->rescanHDAccounts(0, found, error));
    BOOST_CHECK(!gui->rescanHDAccounts(21, found, error));
    BOOST_CHECK_MESSAGE(gui->rescanHDAccounts(1, found, error), error.original);
    BOOST_CHECK(found.empty());
    BOOST_CHECK_EQUAL(gui->listHDAccounts(false).size(), 2U);

    // What a restore of this wallet's phrase would report.
    const interfaces::WalletRestoreReport report{gui->getRestoreReport()};
    BOOST_CHECK_EQUAL(report.hd_type, "bip39");
    BOOST_CHECK_EQUAL(report.first_path, "m/0'/0'/0'");
    BOOST_CHECK(report.history);
    BOOST_CHECK(report.accounts_found == std::vector<uint32_t>{1});
    BOOST_CHECK(!report.scan_owed);
    BOOST_CHECK(!gui->hdAccountsUnlockOwed());
}

BOOST_AUTO_TEST_SUITE_END()
