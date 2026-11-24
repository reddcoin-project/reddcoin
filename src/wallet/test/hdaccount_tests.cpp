// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/chain.h>
#include <key.h>
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

#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {
//! The BIP39 seed of "abandon abandon ... abandon about" with no passphrase.
const std::string VECTOR_SEED{"5eb00bbddcf069084889a8ab9155568165f5c453ccb85e70811aaed6f6da5fc19a5ac40b389cd370d086206dec8aa6c43daea6690f20ad3d8d48b2d2ce9e38e4"};

//! Keys of HD accounts of VECTOR_SEED, computed by an implementation of BIP32
//! independent of this one, which gives 1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA,
//! the widely published address at m/44'/0'/0'/0/0 of that phrase.
struct AccountKeyVector {
    bool bip44;
    uint32_t coin_type;
    uint32_t account;
    bool internal;
    uint32_t index;
    std::string pubkey;
};
const std::vector<AccountKeyVector> ACCOUNT_KEY_VECTORS{
    {true, 4, 0, false, 0, "0309e0c532c4a5c9043ae732bbb801a40e34fe8592543dabc736fa1603460fb713"},
    {true, 4, 0, true, 3, "02c57b4c519a78dfbd3ea63198079b2ffb75cfc4ecd28c67a0bbacb1e6b78fc3d1"},
    {true, 4, 1, false, 0, "039ed9b363c562bfaabe874fc79b565a190d7396085168d9d1e1c9a5caab51f3a7"},
    {true, 4, 1, true, 3, "0397860f3deacfd3a2157a27624aafdb4669d72923672f11683dc99483a03c65c6"},
    {true, 4, 7, false, 0, "02a1068f7c367249e7cbe20c1122bad8a8edd41037864af1d607e2adda6782eaef"},
    {true, 4, 7, true, 3, "0378ac91b265589b75ebf87c07ef7490b528b49a4bfefeccb6f70d40ad7761d6c4"},
    {true, 1, 0, false, 0, "02a7451395735369f2ecdfc829c0f774e88ef1303dfe5b2f04dbaab30a535dfdd6"},
    {true, 1, 0, true, 3, "02193e15345dfe672bc123a7ce53f9388977971e489efe076dae05fcf5045961b1"},
    {true, 1, 1, false, 0, "0283a9e7d8261a2ab969f286208712fd620a6b1d38b525ff0dffff2f8188d2fe47"},
    {true, 1, 1, true, 3, "03af5e09e8bcd868ea40bcf5d238fb45faa4d1ddfbc9628c410b45e1b597d67605"},
    {true, 1, 7, false, 0, "025efd2507b8a6ad84f520fcab6dc6ea412a7ded9c9979b124297f7b91f23610b6"},
    {true, 1, 7, true, 3, "0254863bfc368b0c251cab0b418b97689d924712f9f91cff8972168f64696ca1a1"},
    {false, 0, 0, false, 0, "026fe19b94967c5159c9afaf8eb2870abed5fa0405ae091e4abc69487f7660b8a1"},
    {false, 0, 0, true, 3, "0326643a26d8320d1a8154f2ba66854d4ec00452572ce1d5870b555ff2ea409fce"},
    {false, 0, 1, false, 0, "0351d5f095f156eb04fe3a2dadd0097993cbc90c13bf50e92e7645eb244ad70ce7"},
    {false, 0, 1, true, 3, "03f57fbf486e012d7a2d0988bd5c9a118b82b12fc49be249ef4b024b3e6b82d109"},
    {false, 0, 7, false, 0, "0267642330e33ef85a5f9e2966693cf66603183a96cfd858a65672ab571f6f046b"},
    {false, 0, 7, true, 3, "02b2f1ccc2b87e86120b1ed02d341785e0229f2374b280bac62c72e63bb86698d9"},
};

//! A chain, and wallets of each kind made on it.
struct HDAccountTestingSetup : public TestChain100Setup {
    HDAccountTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
    }

    std::shared_ptr<CWallet> Create(const std::string& name, int wallet_type, uint64_t flags = 0, const SecureString& passphrase = {})
    {
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        options.create_flags = flags;
        options.create_passphrase = passphrase;
        walletoptions.walletType = wallet_type;
        std::shared_ptr<CWallet> wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
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

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
    }

    //! Every form a payment to `pubkey` can take.
    static std::vector<CScript> ScriptsOf(const CPubKey& pubkey)
    {
        const CScript witprog{GetScriptForDestination(WitnessV0KeyHash(pubkey.GetID()))};
        return {
            GetScriptForDestination(PKHash(pubkey)),
            GetScriptForRawPubKey(pubkey),
            witprog,
            GetScriptForDestination(ScriptHash(witprog)),
        };
    }
};

const int WALLET_TYPES[]{walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet};
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdaccount_tests, HDAccountTestingSetup)

// The key of an account is at the native place of each layout, for the coin
// type given, on both chains.
BOOST_AUTO_TEST_CASE(account_key_vectors)
{
    const std::vector<unsigned char> seed{ParseHex(VECTOR_SEED)};
    CExtKey master;
    master.SetSeed(seed.data(), seed.size());
    const uint32_t h{0x80000000};
    for (const AccountKeyVector& v : ACCOUNT_KEY_VECTORS) {
        const HDAccountKey derived{DeriveHDAccountKeyFromMaster(master, v.bip44, v.coin_type, v.account, v.internal, v.index)};
        BOOST_CHECK_EQUAL(HexStr(derived.key.GetPubKey()), v.pubkey);
        const std::vector<uint32_t> path{v.bip44 ?
            std::vector<uint32_t>{44 | h, v.coin_type | h, v.account | h, v.internal ? 1U : 0U, v.index} :
            std::vector<uint32_t>{v.account | h, (v.internal ? 1U : 0U) | h, v.index | h}};
        BOOST_CHECK(derived.path == path);
        BOOST_CHECK_EQUAL(HDAccountOfKeyPath(derived.path), v.account);
        const std::string text{v.bip44 ?
            strprintf("m/44'/%d'/%d'/%d/%d", v.coin_type, v.account, v.internal, v.index) :
            strprintf("m/%d'/%d'/%d'", v.account, v.internal, v.index)};
        BOOST_CHECK_EQUAL(derived.path_text, text);
    }
}

// Account 0 is the chain every wallet already has: each key a wallet derived
// is the key of account 0 at its index, with the same path.
BOOST_AUTO_TEST_CASE(account_zero_is_the_existing_chain)
{
    for (const int wallet_type : WALLET_TYPES) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("zero_%d", wallet_type), wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        for (int i = 0; i < 4; ++i) {
            CTxDestination dest;
            std::string error;
            BOOST_REQUIRE(wallet->GetNewDestination(OutputType::LEGACY, "", dest, error));
            BOOST_REQUIRE(wallet->GetNewChangeDestination(OutputType::LEGACY, dest, error));
        }
        {
            LOCK2(wallet->cs_wallet, spk_man.cs_KeyStore);
            const CKeyID seed_id = spk_man.GetHDChain().seed_id;
            size_t compared{0};
            for (const auto& [key_id, meta] : spk_man.mapKeyMetadata) {
                const std::vector<uint32_t>& path = meta.key_origin.path;
                if (meta.hd_seed_id != seed_id || (path.size() != 3 && path.size() != 5)) continue;
                const bool internal{(path.size() == 5 ? path[3] : path[1] & ~0x80000000) != 0};
                const uint32_t index{path.size() == 5 ? path[4] : path[2] & ~0x80000000};
                CKey key;
                CKeyMetadata derived_meta;
                BOOST_REQUIRE(spk_man.DeriveHDAccountKey(0, internal, index, key, derived_meta));
                BOOST_CHECK(key.GetPubKey().GetID() == key_id);
                BOOST_CHECK(derived_meta.key_origin.path == path);
                BOOST_CHECK_EQUAL(derived_meta.hdKeypath, meta.hdKeypath);
                BOOST_CHECK(derived_meta.hd_seed_id == meta.hd_seed_id);
                BOOST_CHECK(std::equal(std::begin(derived_meta.key_origin.fingerprint), std::end(derived_meta.key_origin.fingerprint), std::begin(meta.key_origin.fingerprint)));
                ++compared;
            }
            // Both chains, the four handed out of each and the key pool behind them.
            BOOST_CHECK_GE(compared, 8U);
        }
        Unload(std::move(wallet));
    }
}

// An account key is derived only through the seed the wallet can read now:
// not while an encrypted wallet is locked, not from a wallet that is not HD,
// and not from one without private keys.
BOOST_AUTO_TEST_CASE(account_key_needs_the_seed)
{
    const SecureString passphrase{"passphrase"};
    for (const int wallet_type : WALLET_TYPES) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("locked_%d", wallet_type), wallet_type, 0, passphrase);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        CKey key;
        CKeyMetadata meta;
        BOOST_REQUIRE(wallet->IsLocked());
        BOOST_CHECK(!WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(1, false, 0, key, meta)));
        BOOST_REQUIRE(wallet->Unlock(passphrase));
        BOOST_CHECK(WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(1, false, 0, key, meta)));
        BOOST_CHECK_EQUAL(HDAccountOfKeyPath(meta.key_origin.path), 1U);
        // Numbers past 31 bits would be those of other keys once hardened.
        BOOST_CHECK(WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(0x7fffffff, false, 0x7fffffff, key, meta)));
        BOOST_CHECK(!WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(0x80000001, false, 0, key, meta)));
        BOOST_CHECK(!WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(1, false, 0x80000000, key, meta)));
        BOOST_REQUIRE(wallet->Lock());
        BOOST_CHECK(!WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(1, false, 0, key, meta)));
        Unload(std::move(wallet));
    }

    std::shared_ptr<CWallet> no_keys = Create("no_private_keys", walletType::bip32Wallet, WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    LegacyScriptPubKeyMan& no_keys_spk_man = *Assert(no_keys->GetOrCreateLegacyScriptPubKeyMan());
    CKey key;
    CKeyMetadata meta;
    BOOST_CHECK(!WITH_LOCK(no_keys_spk_man.cs_KeyStore, return no_keys_spk_man.DeriveHDAccountKey(1, false, 0, key, meta)));
    Unload(std::move(no_keys));

    std::shared_ptr<CWallet> blank = Create("blank", walletType::bip32Wallet, WALLET_FLAG_BLANK_WALLET);
    LegacyScriptPubKeyMan& blank_spk_man = *Assert(blank->GetOrCreateLegacyScriptPubKeyMan());
    BOOST_CHECK(!WITH_LOCK(blank_spk_man.cs_KeyStore, return blank_spk_man.DeriveHDAccountKey(1, false, 0, key, meta)));
    Unload(std::move(blank));
}

// An account key added to the wallet is the wallet's in every form a payment
// to it can take, segwit ones included, from the moment it is added, and is
// written with its path.
BOOST_AUTO_TEST_CASE(added_account_key_is_mine_in_every_form)
{
    const SecureString passphrase{"passphrase"};
    for (const bool encrypted : {false, true}) {
        for (const int wallet_type : WALLET_TYPES) {
            const std::string name{strprintf("added_%d_%d", encrypted, wallet_type)};
            std::shared_ptr<CWallet> wallet = Create(name, wallet_type, 0, encrypted ? passphrase : SecureString{});
            if (encrypted) BOOST_REQUIRE(wallet->Unlock(passphrase));
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            CKey key;
            CKeyMetadata meta;
            {
                LOCK2(wallet->cs_wallet, spk_man.cs_KeyStore);
                BOOST_REQUIRE(spk_man.DeriveHDAccountKey(2, true, 5, key, meta));
                WalletBatch batch(wallet->GetDatabase());
                BOOST_REQUIRE(spk_man.AddHDAccountKey(batch, key, meta));
                // Added a second time, it is left as it is.
                BOOST_REQUIRE(spk_man.AddHDAccountKey(batch, key, meta));
            }
            const CPubKey pubkey{key.GetPubKey()};
            for (const CScript& script : ScriptsOf(pubkey)) {
                BOOST_CHECK_EQUAL(spk_man.IsMine(script), ISMINE_SPENDABLE);
            }
            BOOST_CHECK_EQUAL(WITH_LOCK(spk_man.cs_KeyStore, return HDAccountOfKeyPath(spk_man.mapKeyMetadata.at(pubkey.GetID()).key_origin.path)), 2U);
            if (encrypted) BOOST_REQUIRE(wallet->Lock());
            Unload(std::move(wallet));

            // A five-element path of any account is read back by the loader as
            // it is today; a three-element one only once the loader accepts an
            // account other than 0 there.
            if (wallet_type != walletType::bip44Wallet) continue;
            wallet = Load(name);
            LegacyScriptPubKeyMan& loaded = *Assert(wallet->GetLegacyScriptPubKeyMan());
            for (const CScript& script : ScriptsOf(pubkey)) {
                BOOST_CHECK_EQUAL(loaded.IsMine(script), ISMINE_SPENDABLE);
            }
            BOOST_CHECK(WITH_LOCK(loaded.cs_KeyStore, return loaded.mapKeyMetadata.at(pubkey.GetID()).key_origin.path) == meta.key_origin.path);
            Unload(std::move(wallet));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
