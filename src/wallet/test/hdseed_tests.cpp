// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <outputtype.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <validationinterface.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>
#include <support/allocators/secure.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {
//! A chain, and wallets of each kind made on it.
struct HDSeedTestingSetup : public TestChain100Setup {
    HDSeedTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
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

    //! A new address of the wallet, as the script that pays it.
    CScript NewScript(CWallet& wallet)
    {
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet.GetNewDestination(OutputType::LEGACY, "", dest, error));
        return GetScriptForDestination(dest);
    }

    //! What sethdseed does: a new seed in the place of the wallet's, and a new key pool.
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
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdseed_tests, HDSeedTestingSetup)

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
