// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>
#include <univalue.h>
#include <support/allocators/secure.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace {
//! A chain, the wallet commands, and wallets made the way the wallet creation
//! dialog makes them: this version has no command that creates a wallet from
//! a mnemonic.
struct HDSeedTestingSetup : public TestChain100Setup {
    HDSeedTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
        m_wallet_client->registerRpcs();
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    }

    std::shared_ptr<CWallet> Create(const std::string& name, int wallet_type, uint64_t flags = 0)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        options.create_flags = flags;
        walletoptions.walletType = wallet_type;
        std::shared_ptr<CWallet> wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
    }

    //! Run a wallet command the way the RPC server does.
    UniValue Call(const CWallet& wallet, const std::string& method, const UniValue& params = UniValue(UniValue::VARR))
    {
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = params;
        request.URI = "/wallet/" + wallet.GetName();
        try {
            return tableRPC.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(strprintf("%d %s", find_value(error, "code").get_int(), find_value(error, "message").get_str()));
        }
    }

    //! What a command fails with, code and message, or nothing.
    std::string Failure(const CWallet& wallet, const std::string& method, const UniValue& params)
    {
        try {
            Call(wallet, method, params);
        } catch (const std::runtime_error& error) {
            return error.what();
        }
        return "";
    }

    //! The path of the key of a new address.
    std::string NextPath(CWallet& wallet)
    {
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet.GetNewDestination(OutputType::LEGACY, "", dest, error));
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK(spk_man.cs_KeyStore);
        return spk_man.mapKeyMetadata.at(ToKeyID(std::get<PKHash>(dest))).hdKeypath;
    }

    std::shared_ptr<CWallet> Load(const std::string& name, bilingual_str& error)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        return LoadWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, status, error, warnings);
    }

    //! A new address of the wallet, as the script that pays it.
    CScript NewScript(CWallet& wallet)
    {
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet.GetNewDestination(OutputType::LEGACY, "", dest, error));
        return GetScriptForDestination(dest);
    }

    //! Pay a script with a coinbase of the chain, in a new block. Returns the amount.
    CAmount Pay(const CScript& script)
    {
        // The fixture's chain is staked past height 89, and its staker has
        // already spent some of the coinbases, so take the next one that is
        // unspent and mature.
        const CTransaction* from_ptr{nullptr};
        {
            LOCK(cs_main);
            const int next_height = m_node.chainman->ActiveChain().Height() + 1;
            while (!from_ptr) {
                const CTransactionRef& tx = m_coinbase_txns.at(m_coinbases_spent++);
                if (!tx->IsCoinBase()) continue;
                const Coin& coin = m_node.chainman->ActiveChainstate().CoinsTip().AccessCoin(COutPoint(tx->GetHash(), 0));
                if (coin.IsSpent() || next_height - (int)coin.nHeight < Params().GetConsensus().GetCoinbaseMaturity()) continue;
                from_ptr = tx.get();
            }
        }
        const CTransaction& from = *from_ptr;
        CMutableTransaction mtx;
        mtx.vout.push_back({from.vout[0].nValue - DEFAULT_TRANSACTION_MAXFEE, script});
        mtx.vin.push_back({CTxIn{from.GetHash(), 0}});
        FillableSigningProvider keystore;
        keystore.AddKey(coinbaseKey);
        std::map<COutPoint, Coin> coins;
        coins[mtx.vin[0].prevout].out = from.vout[0];
        std::map<int, std::string> input_errors;
        BOOST_REQUIRE(SignTransaction(mtx, &keystore, coins, SIGHASH_ALL, input_errors));
        CreateAndProcessPoSBlock({mtx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), m_wallet);
        SyncWithValidationInterfaceQueue();
        return mtx.vout[0].nValue;
    }

    //! What sethdseed did before it refused a wallet created from a mnemonic.
    CKeyID ReplaceSeed(CWallet& wallet)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK2(wallet.cs_wallet, spk_man.cs_KeyStore);
        spk_man.SetHDSeed(spk_man.GenerateNewSeed());
        BOOST_REQUIRE(spk_man.NewKeyPool());
        return spk_man.GetHDChain().seed_id;
    }

    //! The keys of account 0 of a seed that the wallet holds: how many, and the last of each chain.
    struct SeedKeys {
        size_t count{0};
        int64_t last_external{-1};
        int64_t last_internal{-1};
        CKeyID last_external_key;
    };
    SeedKeys KeysOf(const CWallet& wallet, const CKeyID& seed_id)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK(spk_man.cs_KeyStore);
        SeedKeys keys;
        for (const auto& [key_id, meta] : spk_man.mapKeyMetadata) {
            const std::vector<uint32_t>& path = meta.key_origin.path;
            if (meta.hd_seed_id != seed_id || (path.size() != 3 && path.size() != 5) || HDAccountOfKeyPath(path) != 0) continue;
            ++keys.count;
            const bool bip44{path.size() == 5};
            const bool internal{(bip44 ? path[3] : path[1] & ~0x80000000) != 0};
            const int64_t index{bip44 ? path[4] : path[2] & ~0x80000000};
            if (internal) {
                keys.last_internal = std::max(keys.last_internal, index);
            } else if (index > keys.last_external) {
                keys.last_external = index;
                keys.last_external_key = key_id;
            }
        }
        return keys;
    }

    size_t m_coinbases_spent{0};
    std::unique_ptr<interfaces::WalletClient> m_wallet_client = interfaces::MakeWalletClient(*m_node.chain, *Assert(m_node.args));

    //! The path of key `index` on the receive chain of HD account `account`,
    //! in the layout of a wallet of the given type.
    static std::vector<uint32_t> AccountPath(int wallet_type, uint32_t account, uint32_t index)
    {
        const uint32_t h{0x80000000};
        if (wallet_type == walletType::bip44Wallet) {
            return {44 | h, static_cast<uint32_t>(Params().ExtCoinType()) | h, account | h, 0, index};
        }
        return {account | h, 0 | h, index | h};
    }

    //! The text form of a path, as hdKeypath holds it.
    static std::string PathText(const std::vector<uint32_t>& path)
    {
        std::string text{"m"};
        for (const uint32_t element : path) {
            text += strprintf("/%d%s", element & ~0x80000000, (element & 0x80000000) ? "'" : "");
        }
        return text;
    }

    std::shared_ptr<CWallet> Load(const std::string& name)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet = LoadWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }
};

UniValue Params(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const UniValue& value : values) params.push_back(value);
    return params;
}

std::string NewSecret()
{
    CKey key;
    key.MakeNewKey(/* compressed */ true);
    return EncodeSecret(key);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sethdseed_tests, HDSeedTestingSetup)

// sethdseed on a wallet created from a mnemonic is refused, whatever its
// arguments, and leaves the wallet as it was.
BOOST_AUTO_TEST_CASE(refused_on_a_wallet_created_from_a_mnemonic)
{
    for (const int wallet_type : {walletType::bip39Wallet, walletType::bip44Wallet}) {
        const bool bip44{wallet_type == walletType::bip44Wallet};
        std::shared_ptr<CWallet> wallet = Create(bip44 ? "bip44" : "bip39", wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.IsBip39Enabled());
        BOOST_CHECK_EQUAL(spk_man.IsBip44Enabled(), bip44);
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/0" : "m/0'/0'/0'");
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/1" : "m/0'/0'/1'");

        const CHDChain before = spk_man.GetHDChain();
        const std::set<CKeyID> keys_before = spk_man.GetKeys();
        const unsigned int keypool_before = WITH_LOCK(wallet->cs_wallet, return wallet->GetKeyPoolSize());
        const std::string info_before = Call(*wallet, "gethdwalletinfo").write();
        BOOST_CHECK(!before.vchMnemonic.empty());
        BOOST_CHECK(info_before.find("\"mnemonic\"") != std::string::npos);

        for (const UniValue& params : {Params({}), Params({true}), Params({false}), Params({true, NewSecret()}), Params({false, NewSecret()})}) {
            const std::string failure = Failure(*wallet, "sethdseed", params);
            BOOST_CHECK_MESSAGE(failure.find(strprintf("%d Cannot set a new HD seed on a wallet created from a mnemonic", RPC_WALLET_ERROR)) == 0, failure);
        }

        const CHDChain& after = spk_man.GetHDChain();
        BOOST_CHECK(after.seed_id == before.seed_id);
        BOOST_CHECK(after.vchMnemonic == before.vchMnemonic);
        BOOST_CHECK(after.vchMnemonicPassphrase == before.vchMnemonicPassphrase);
        BOOST_CHECK_EQUAL(after.IsBip44(), bip44);
        BOOST_CHECK_EQUAL(after.nVersion, before.nVersion);
        BOOST_CHECK_EQUAL(after.nExternalChainCounter, before.nExternalChainCounter);
        BOOST_CHECK_EQUAL(after.nInternalChainCounter, before.nInternalChainCounter);
        BOOST_CHECK(spk_man.GetKeys() == keys_before);
        BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->GetKeyPoolSize()), keypool_before);
        BOOST_CHECK_EQUAL(Call(*wallet, "gethdwalletinfo").write(), info_before);
        // The next address is the next one of the phrase.
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/2" : "m/0'/0'/2'");
        Unload(std::move(wallet));
    }
}

// It goes on working on a wallet with a plain HD seed, and on a blank one.
BOOST_AUTO_TEST_CASE(accepted_on_a_plain_seed_and_on_a_blank_wallet)
{
    std::shared_ptr<CWallet> wallet = Create("bip32", walletType::bip32Wallet);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(spk_man.IsHDEnabled() && !spk_man.IsBip39Enabled());
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/1'");
    const CKeyID first = spk_man.GetHDChain().seed_id;
    const std::set<CKeyID> keys = spk_man.GetKeys();

    BOOST_CHECK(Call(*wallet, "sethdseed").isNull());
    const CKeyID second = spk_man.GetHDChain().seed_id;
    BOOST_CHECK(second != first);
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    // The keys of the first seed are still the wallet's.
    for (const CKeyID& key : keys) BOOST_CHECK(spk_man.HaveKey(key));

    CKey seed;
    seed.MakeNewKey(/* compressed */ true);
    BOOST_CHECK(Call(*wallet, "sethdseed", Params({true, EncodeSecret(seed)})).isNull());
    BOOST_CHECK(spk_man.GetHDChain().seed_id == seed.GetPubKey().GetID());
    BOOST_CHECK(!spk_man.IsBip39Enabled());
    Unload(std::move(wallet));

    wallet = Create("blank", walletType::bip32Wallet, WALLET_FLAG_BLANK_WALLET);
    LegacyScriptPubKeyMan& blank = *Assert(wallet->GetOrCreateLegacyScriptPubKeyMan());
    BOOST_CHECK(!blank.IsHDEnabled());
    BOOST_CHECK(Call(*wallet, "sethdseed").isNull());
    BOOST_CHECK(blank.IsHDEnabled() && !blank.IsBip39Enabled());
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    Unload(std::move(wallet));
}

// A wallet created from a mnemonic on which sethdseed was run before it was
// refused. What is left once it has been closed is the keys it had derived
// from the phrase, and nothing to derive more of them from. It loads although
// one of those keys has been paid, it is credited with a payment to one of
// them while it is loaded, and it can spend both. It does not have its phrase,
// and it gets no further key of it.
BOOST_AUTO_TEST_CASE(wallet_that_cannot_derive_old_keys_loads_and_is_paid)
{
    for (const int wallet_type : {walletType::bip44Wallet, walletType::bip39Wallet}) {
        const std::string name{wallet_type == walletType::bip44Wallet ? "replaced_bip44" : "replaced_bip39"};
        std::shared_ptr<CWallet> wallet = Create(name, wallet_type);
        const CKeyID old_seed = Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDChain().seed_id;
        const std::vector<CScript> old{NewScript(*wallet), NewScript(*wallet), NewScript(*wallet)};
        const CKeyID new_seed = ReplaceSeed(*wallet);
        const SeedKeys old_keys = KeysOf(*wallet, old_seed);
        BOOST_CHECK_GT(old_keys.count, 5U);
        Unload(std::move(wallet));

        // Paid while it is not loaded: the wallet meets the payment as it loads.
        const CAmount first = Pay(old[2]);
        bilingual_str error;
        bool said{false};
        {
            DebugLogHelper log("cannot derive keys of HD seed " + old_seed.GetHex(), [&](const std::string* line) {
                if (line) said = true;
                return false;
            });
            wallet = Load(name, error);
        }
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK(said);
        BOOST_CHECK_EQUAL(wallet->GetBalance().m_mine_trusted, first);

        // Paid while it is loaded.
        const CAmount second = Pay(old[1]);
        BOOST_CHECK_EQUAL(wallet->GetBalance().m_mine_trusted, first + second);

        // Both coins are spent by one payment.
        CTransactionRef tx;
        CAmount fee;
        int change_position = -1;
        FeeCalculation fee_calculation;
        const CAmount amount = first + second / 2;
        BOOST_REQUIRE_MESSAGE(wallet->CreateTransaction({{GetScriptForRawPubKey(coinbaseKey.GetPubKey()), amount, /* subtract fee */ false}}, tx, fee, change_position, error, CCoinControl{}, fee_calculation), error.original);
        BOOST_CHECK_EQUAL(tx->vin.size(), 2U);
        wallet->CommitTransaction(tx, {}, {});
        CreateAndProcessPoSBlock({CMutableTransaction(*tx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), m_wallet);
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK_EQUAL(wallet->GetBalance().m_mine_trusted, first + second - amount - fee);

        // What it cannot do. It has a plain seed, the new one, and no phrase.
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDChain().seed_id == new_seed);
        BOOST_CHECK(spk_man.GetHDChain().vchMnemonic.empty());
        BOOST_CHECK(!spk_man.IsBip39Enabled());
        BOOST_CHECK(Call(*wallet, "gethdwalletinfo").write().find("\"mnemonic\"") == std::string::npos);
        // Its new addresses are the new seed's.
        BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
        // It has the keys of the phrase that it had, and not one more.
        const SeedKeys now = KeysOf(*wallet, old_seed);
        BOOST_CHECK_EQUAL(now.count, old_keys.count);
        BOOST_CHECK_EQUAL(now.last_external, old_keys.last_external);
        BOOST_CHECK_EQUAL(now.last_internal, old_keys.last_internal);

        Unload(std::move(wallet));
        wallet = Load(name, error);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK_EQUAL(wallet->GetBalance().m_mine_trusted, first + second - amount - fee);
        Unload(std::move(wallet));
    }
}

// The account of a key is read from the length of its path: the third of five
// elements, the first of three, and 0 for any other length.
BOOST_AUTO_TEST_CASE(hd_account_of_key_path)
{
    const uint32_t h{0x80000000};
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({0 | h, 0 | h, 7 | h}), 0U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({3 | h, 1 | h, 7 | h}), 3U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({44 | h, 4 | h, 0 | h, 0, 7}), 0U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({44 | h, 4 | h, 2 | h, 1, 7}), 2U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({44 | h, 1 | h, 0x7fffffff | h, 0, 0}), 0x7fffffffU);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({}), 0U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({5 | h}), 0U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({5 | h, 1 | h, 2 | h, 3 | h}), 0U);
    BOOST_CHECK_EQUAL(HDAccountOfKeyPath({44 | h, 4 | h, 2 | h, 0, 7, 9}), 0U);
}

// A chain that is put together at load from the metadata of its keys, as the
// chain of a replaced seed is, counts the keys of account 0 only. A key of
// another account, however far along its own chain, does not move the
// counters of account 0.
BOOST_AUTO_TEST_CASE(rebuilt_chain_counts_account_zero_only)
{
    const std::string name{"rebuilt"};
    std::shared_ptr<CWallet> wallet = Create(name, walletType::bip44Wallet);
    const CKeyID replaced = Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDChain().seed_id;
    NewScript(*wallet);
    ReplaceSeed(*wallet);
    Unload(std::move(wallet));

    wallet = Load(name);
    BOOST_REQUIRE_EQUAL(Assert(wallet->GetLegacyScriptPubKeyMan())->GetInactiveHDChains().count(replaced), 1U);
    const CHDChain before = Assert(wallet->GetLegacyScriptPubKeyMan())->GetInactiveHDChains().at(replaced);
    CKeyMetadata meta;
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        LOCK(spk_man.cs_KeyStore);
        for (const auto& meta_pair : spk_man.mapKeyMetadata) {
            if (meta_pair.second.hd_seed_id == replaced && meta_pair.second.key_origin.path.size() == 5) meta = meta_pair.second;
        }
    }
    BOOST_REQUIRE(!meta.hd_seed_id.IsNull());
    Unload(std::move(wallet));

    // A key of account 2 on each chain of the replaced seed, far beyond account 0's counters.
    for (const uint32_t chain : {0U, 1U}) {
        CKey key;
        key.MakeNewKey(true);
        meta.key_origin.path = AccountPath(walletType::bip44Wallet, 2, 500);
        meta.key_origin.path[3] = chain;
        meta.hdKeypath = PathText(meta.key_origin.path);
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        options.require_existing = true;
        std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
        BOOST_REQUIRE(database);
        BOOST_REQUIRE(WalletBatch(*database).WriteKeyMetadata(meta, key.GetPubKey(), /* overwrite */ false));
    }

    wallet = Load(name);
    const CHDChain after = Assert(wallet->GetLegacyScriptPubKeyMan())->GetInactiveHDChains().at(replaced);
    BOOST_CHECK_EQUAL(after.nExternalChainCounter, before.nExternalChainCounter);
    BOOST_CHECK_EQUAL(after.nInternalChainCounter, before.nInternalChainCounter);
    BOOST_CHECK_LT(after.nExternalChainCounter, 500U);
    Unload(std::move(wallet));
}

// When a key of a replaced seed is used, further keys of account 0's chain are
// held ready behind it. A key of another account of that seed says nothing of
// where account 0 is: no key of account 0 is derived for it.
BOOST_AUTO_TEST_CASE(inactive_chain_is_topped_up_for_account_zero_keys_only)
{
    for (const int wallet_type : {walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet}) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("account_topup_%d", wallet_type), wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        const CKeyID old_seed = spk_man.GetHDChain().seed_id;
        NewScript(*wallet);
        ReplaceSeed(*wallet);
        const SeedKeys before = KeysOf(*wallet, old_seed);
        BOOST_REQUIRE(before.last_external >= 0);

        // A key of account 2 of the replaced seed, at an index past account 0's keys.
        CKey key;
        key.MakeNewKey(true);
        {
            LOCK(wallet->cs_wallet);
            BOOST_REQUIRE(spk_man.AddKeyPubKey(key, key.GetPubKey()));
            LOCK(spk_man.cs_KeyStore);
            CKeyMetadata meta{spk_man.mapKeyMetadata.at(before.last_external_key)};
            meta.key_origin.path = AccountPath(wallet_type, 2, before.last_external + 10);
            meta.hdKeypath = PathText(meta.key_origin.path);
            spk_man.mapKeyMetadata[key.GetPubKey().GetID()] = meta;
        }

        WITH_LOCK(wallet->cs_wallet, spk_man.MarkUnusedAddresses(GetScriptForDestination(PKHash(key.GetPubKey()))));

        const SeedKeys after = KeysOf(*wallet, old_seed);
        BOOST_CHECK_EQUAL(after.count, before.count);
        BOOST_CHECK_EQUAL(after.last_external, before.last_external);
        BOOST_CHECK_EQUAL(after.last_internal, before.last_internal);

        // A key of account 0 of that seed is still topped up behind.
        WITH_LOCK(wallet->cs_wallet, spk_man.MarkUnusedAddresses(GetScriptForDestination(PKHash(before.last_external_key))));
        BOOST_CHECK_EQUAL(KeysOf(*wallet, old_seed).last_external, before.last_external + 3);
        Unload(std::move(wallet));
    }
}

// A seed that was replaced in this session can still be derived from. When
// one of its keys is used, further keys of the chain that key is on are held
// ready behind it: for a BIP44 path, m/44'/coin'/account'/chain/index, the
// chain and the index are the last two elements.
BOOST_AUTO_TEST_CASE(replaced_seed_is_topped_up_on_the_chain_of_the_key_used)
{
    for (const int wallet_type : {walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet}) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("topup_%d", wallet_type), wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        const CKeyID old_seed = spk_man.GetHDChain().seed_id;
        NewScript(*wallet);
        ReplaceSeed(*wallet);
        const SeedKeys before = KeysOf(*wallet, old_seed);
        BOOST_REQUIRE(before.last_external >= 0 && before.last_internal >= 0);

        WITH_LOCK(wallet->cs_wallet, spk_man.MarkUnusedAddresses(GetScriptForDestination(PKHash(before.last_external_key))));

        // -keypool=3 keys behind the one used, on the external chain only.
        const SeedKeys after = KeysOf(*wallet, old_seed);
        BOOST_CHECK_EQUAL(after.last_external, before.last_external + 3);
        BOOST_CHECK_EQUAL(after.last_internal, before.last_internal);
        BOOST_CHECK_EQUAL(after.count, before.count + 3);
        Unload(std::move(wallet));
    }
}

// A chain whose counters are behind its keys, as after a wallet is put back
// from an older copy of its chain record, passes over the indexes that are
// taken. The key it arrives at has the path of its own index, and nothing more.
BOOST_AUTO_TEST_CASE(key_path_when_indexes_are_taken)
{
    for (const int wallet_type : {walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet}) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("taken_%d", wallet_type), wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        for (int i = 0; i < 4; ++i) NewScript(*wallet);
        const SeedKeys before = KeysOf(*wallet, spk_man.GetHDChain().seed_id);

        CHDChain chain = spk_man.GetHDChain();
        chain.nExternalChainCounter = 0;
        chain.nInternalChainCounter = 0;
        spk_man.LoadHDChain(chain);
        for (int i = 0; i < 6; ++i) NewScript(*wallet);

        const SeedKeys after = KeysOf(*wallet, spk_man.GetHDChain().seed_id);
        BOOST_CHECK_GT(after.last_external, before.last_external);
        {
            LOCK(spk_man.cs_KeyStore);
            for (const auto& meta_pair : spk_man.mapKeyMetadata) {
                const CKeyMetadata& meta = meta_pair.second;
                if (meta.hd_seed_id.IsNull() || meta.hdKeypath == "s") continue;
                BOOST_CHECK_EQUAL(meta.key_origin.path.size(), wallet_type == walletType::bip44Wallet ? 5U : 3U);
            }
        }
        Unload(std::move(wallet));
    }
}

// Earlier versions lengthened the stored path of a key each time derivation
// passed over a taken index. A wallet holding such a record loaded with a
// warning, and the wallet tool would not read it. The path is put right from
// its text form when the wallet is loaded, and written back.
BOOST_AUTO_TEST_CASE(overlong_key_path_is_put_right_at_load)
{
    for (const int wallet_type : {walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet}) {
        const std::string name{strprintf("longpath_%d", wallet_type)};
        std::shared_ptr<CWallet> wallet = Create(name, wallet_type);
        const CScript script = NewScript(*wallet);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        CKeyID key_id;
        CKeyMetadata meta;
        {
            LOCK(spk_man.cs_KeyStore);
            CTxDestination dest;
            BOOST_REQUIRE(ExtractDestination(script, dest));
            key_id = ToKeyID(std::get<PKHash>(dest));
            meta = spk_man.mapKeyMetadata.at(key_id);
        }
        const std::vector<uint32_t> right = meta.key_origin.path;
        BOOST_REQUIRE(right.size() == 3 || right.size() == 5);
        CPubKey pubkey;
        BOOST_REQUIRE(spk_man.GetPubKey(key_id, pubkey));
        // As three tries would have left it: the paths of the two passed over, then its own.
        CKeyMetadata long_meta{meta};
        long_meta.key_origin.path.clear();
        for (int i = 0; i < 3; ++i) long_meta.key_origin.path.insert(long_meta.key_origin.path.end(), right.begin(), right.end());
        Unload(std::move(wallet));
        {
            DatabaseOptions options;
            DatabaseStatus status;
            bilingual_str error;
            options.require_existing = true;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
            BOOST_REQUIRE(database);
            WalletBatch batch(*database);
            BOOST_REQUIRE(batch.WriteKeyMetadata(long_meta, pubkey, /* overwrite */ true));
        }

        for (int load = 0; load < 2; ++load) {
            DatabaseOptions options;
            DatabaseStatus status;
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            options.require_existing = true;
            wallet = LoadWallet(*m_node.chain, name, std::nullopt, options, status, error, warnings);
            BOOST_REQUIRE_MESSAGE(wallet, error.original);
            const std::string warning{warnings.empty() ? "" : warnings[0].original};
            BOOST_CHECK_MESSAGE(warnings.empty(), warning);
            LegacyScriptPubKeyMan& loaded = *Assert(wallet->GetLegacyScriptPubKeyMan());
            BOOST_CHECK(WITH_LOCK(loaded.cs_KeyStore, return loaded.mapKeyMetadata.at(key_id).key_origin.path) == right);
            Unload(std::move(wallet));
        }
    }
}

// The seed of a wallet with a plain HD seed is one of its keys. A chain keeps
// a copy of it when an unencrypted wallet is loaded. Once the wallet is
// encrypted no chain holds that copy, neither the new one nor the one that was
// replaced, and the replaced seed can still be derived from when unlocked.
BOOST_AUTO_TEST_CASE(encrypted_wallet_holds_no_seed_copy_in_its_chains)
{
    const SecureString passphrase{"passphrase"};
    std::shared_ptr<CWallet> wallet = Create("plain", walletType::bip32Wallet);
    NewScript(*wallet);
    Unload(std::move(wallet));

    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    options.require_existing = true;
    wallet = LoadWallet(*m_node.chain, "plain", /* load_on_start */ std::nullopt, options, status, error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    const CKeyID old_seed = spk_man.GetHDChain().seed_id;
    // The state this is about: the copy is there.
    BOOST_CHECK_EQUAL(spk_man.GetHDChain().vchSeed.size(), 32U);

    const auto no_copy = [&spk_man] {
        if (!spk_man.GetHDChain().vchSeed.empty()) return false;
        for (const auto& chain_pair : spk_man.GetInactiveHDChains()) {
            if (!chain_pair.second.vchSeed.empty()) return false;
        }
        return true;
    };

    BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(spk_man.GetHDChain().seed_id != old_seed);
    BOOST_CHECK_EQUAL(spk_man.GetInactiveHDChains().count(old_seed), 1U);
    BOOST_CHECK(no_copy());

    // Unlocked, the replaced seed is derived from through the key itself.
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    const SeedKeys before = KeysOf(*wallet, old_seed);
    WITH_LOCK(wallet->cs_wallet, spk_man.MarkUnusedAddresses(GetScriptForDestination(PKHash(before.last_external_key))));
    BOOST_CHECK_EQUAL(KeysOf(*wallet, old_seed).last_external, before.last_external + 3);
    NewScript(*wallet);
    BOOST_CHECK(no_copy());
    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK(no_copy());
    Unload(std::move(wallet));
}

BOOST_AUTO_TEST_SUITE_END()
