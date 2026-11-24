// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

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
#include <wallet/salvage.h>
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
        // A record the loader could not read is a warning, not a failure.
        const std::string warning{warnings.empty() ? "" : warnings.front().original};
        BOOST_CHECK_MESSAGE(warnings.empty(), warning);
        return wallet;
    }

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
    }

    //! Add key `index` of a chain of HD account `account` to an unlocked wallet; its public key.
    static CPubKey AddAccountKey(CWallet& wallet, uint32_t account, bool internal, uint32_t index)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK2(wallet.cs_wallet, spk_man.cs_KeyStore);
        CKey key;
        CKeyMetadata meta;
        BOOST_REQUIRE(spk_man.DeriveHDAccountKey(account, internal, index, key, meta));
        WalletBatch batch(wallet.GetDatabase());
        BOOST_REQUIRE(spk_man.AddHDAccountKey(batch, key, meta));
        return key.GetPubKey();
    }

    //! Mark the wallet as holding HD accounts.
    static bool Setup(CWallet& wallet)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        return WITH_LOCK(wallet.cs_wallet, return spk_man.SetupHDAccounts());
    }

    //! The P2PKH script of key `index` of a chain of `account`, from the keys the wallet holds.
    static CScript AccountScript(const CWallet& wallet, uint32_t account, bool internal, uint32_t index)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK(spk_man.cs_KeyStore);
        for (const auto& [key_id, meta] : spk_man.mapKeyMetadata) {
            const std::vector<uint32_t>& path = meta.key_origin.path;
            if (path.size() != 3 && path.size() != 5) continue;
            const bool key_internal{(path.size() == 5 ? path[3] : path[1] & ~0x80000000) != 0};
            const uint32_t key_index{path.size() == 5 ? path[4] : path[2] & ~0x80000000};
            if (HDAccountOfKeyPath(path) == account && key_internal == internal && key_index == index) {
                return GetScriptForDestination(PKHash(key_id));
            }
        }
        BOOST_FAIL("no such account key");
        return {};
    }

    //! What the wallet does when a transaction pays `script`.
    static void Pay(CWallet& wallet, const CScript& script)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        WITH_LOCK(wallet.cs_wallet, spk_man.MarkUnusedAddresses(script));
    }

    //! The database of a wallet that is not loaded.
    static std::unique_ptr<WalletDatabase> Database(const std::string& name)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        options.require_existing = true;
        std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
        BOOST_REQUIRE_MESSAGE(database, error.original);
        return database;
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

//! An account record as CHDAccount lays it out, in a state no version knows.
struct UnknownStateRecord {
    int nVersion{CHDAccount::CURRENT_VERSION};
    uint8_t state{7};
    int64_t nCreateTime{0};
    std::string label;
    uint32_t next_receive{0};
    uint32_t next_change{0};
    SERIALIZE_METHODS(UnknownStateRecord, obj) { READWRITE(obj.nVersion, obj.state, obj.nCreateTime, obj.label, obj.next_receive, obj.next_change); }
};

const int WALLET_TYPES[]{walletType::bip44Wallet, walletType::bip39Wallet, walletType::bip32Wallet};

//! Whether the transactions of a FailCommitDatabase fail to commit.
bool g_fail_commit{false};

//! A database that keeps nothing, and whose transactions can be made to fail.
class FailCommitBatch : public DummyBatch
{
    bool ReadKey(CDataStream&& key, CDataStream& value) override { return false; }
    bool HasKey(CDataStream&& key) override { return false; }

public:
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override
    {
        complete = true;
        return true;
    }
    bool TxnCommit() override { return !g_fail_commit; }
};

class FailCommitDatabase : public DummyDatabase
{
public:
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override { return std::make_unique<FailCommitBatch>(); }
};
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

            // Read back with its path, in both forms: m/44'/coin'/account'/chain/index
            // and m/account'/chain'/index'.
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

// The records of HD accounts are read back as they were written, and the
// wallet loads without counting them as records it does not know.
BOOST_AUTO_TEST_CASE(account_records_round_trip)
{
    std::shared_ptr<CWallet> wallet = Create("records", walletType::bip39Wallet);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    CHDAccount savings;
    savings.state = CHDAccount::State::ACTIVE;
    savings.nCreateTime = 1700000000;
    savings.label = "savings";
    savings.next_receive = 7;
    savings.next_change = 2;
    CHDAccount pooled;
    pooled.state = CHDAccount::State::POOL;
    CHDAccount retired;
    retired.state = CHDAccount::State::RETIRED;
    retired.label = "old";
    retired.next_receive = 1;
    CHDAccountMeta meta;
    meta.highest_derived = 22;
    meta.highest_used = 2;
    {
        WalletBatch batch(wallet->GetDatabase());
        BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 1, savings));
        BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 2, retired));
        BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 22, pooled));
        BOOST_REQUIRE(spk_man.WriteHDAccountMeta(batch, meta));
    }
    Unload(std::move(wallet));

    wallet = Load("records");
    LegacyScriptPubKeyMan& loaded = *Assert(wallet->GetLegacyScriptPubKeyMan());
    const std::map<uint32_t, CHDAccount> accounts{loaded.GetHDAccounts()};
    BOOST_REQUIRE_EQUAL(accounts.size(), 3U);
    BOOST_CHECK(accounts.at(1) == savings);
    BOOST_CHECK(accounts.at(2) == retired);
    BOOST_CHECK(accounts.at(22) == pooled);
    BOOST_REQUIRE(loaded.GetHDAccountMeta());
    BOOST_CHECK(*loaded.GetHDAccountMeta() == meta);
    Unload(std::move(wallet));

    // A record no version writes, for account 0, an account past 31 bits, or
    // in a state not known, is not taken: the load says data may be missing.
    {
        std::unique_ptr<WalletDatabase> database = Database("records");
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::HDACCOUNT, uint32_t{0}), savings));
        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::HDACCOUNT, uint32_t{0x80000003}), savings));
        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::HDACCOUNT, uint32_t{4}), UnknownStateRecord{}));
    }
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    options.require_existing = true;
    wallet = LoadWallet(*m_node.chain, "records", std::nullopt, options, status, error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    BOOST_CHECK(!warnings.empty());
    const std::map<uint32_t, CHDAccount> checked{Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccounts()};
    BOOST_CHECK_EQUAL(checked.size(), 3U);
    BOOST_CHECK_EQUAL(checked.count(0), 0U);
    BOOST_CHECK_EQUAL(checked.count(4), 0U);
    BOOST_CHECK_EQUAL(checked.count(0x80000003), 0U);
    Unload(std::move(wallet));
}

// The record of an account whose keys are in the wallet and whose record is
// not, as after it was lost, is put together from the paths of its keys and
// written: the account is taken as handed out, its next indexes where its
// keys end less the look-ahead derived past them. A record that is there is
// left as it is, and keys of account 0 make no record.
BOOST_AUTO_TEST_CASE(lost_account_records_are_rebuilt)
{
    for (const int wallet_type : WALLET_TYPES) {
        const std::string name{strprintf("rebuild_%d", wallet_type)};
        std::shared_ptr<CWallet> wallet = Create(name, wallet_type);
        // As the look-ahead leaves them: receive keys 3 + 20, change keys 1 + 20.
        for (uint32_t i = 0; i < 3 + HD_ACCOUNT_KEY_LOOKAHEAD; ++i) AddAccountKey(*wallet, 1, false, i);
        for (uint32_t i = 0; i < 1 + HD_ACCOUNT_KEY_LOOKAHEAD; ++i) AddAccountKey(*wallet, 1, true, i);
        AddAccountKey(*wallet, 3, false, 4);
        AddAccountKey(*wallet, 5, true, 6);
        CHDAccount kept;
        kept.state = CHDAccount::State::POOL;
        kept.label = "kept";
        kept.next_receive = 1;
        {
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            WalletBatch batch(wallet->GetDatabase());
            BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 5, kept));
        }
        Unload(std::move(wallet));

        for (int load = 0; load < 2; ++load) {
            wallet = Load(name);
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            const std::map<uint32_t, CHDAccount> accounts{spk_man.GetHDAccounts()};
            BOOST_REQUIRE_EQUAL(accounts.size(), 3U);
            BOOST_CHECK(accounts.at(1).state == CHDAccount::State::ACTIVE);
            BOOST_CHECK_EQUAL(accounts.at(1).next_receive, 3U);
            BOOST_CHECK_EQUAL(accounts.at(1).next_change, 1U);
            // Fewer keys than the look-ahead: nothing handed out.
            BOOST_CHECK_EQUAL(accounts.at(3).next_receive, 0U);
            BOOST_CHECK_EQUAL(accounts.at(3).next_change, 0U);
            BOOST_CHECK(accounts.at(5) == kept);
            BOOST_REQUIRE(spk_man.GetHDAccountMeta());
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_derived, 5U);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_used, 3U);
            // Account 0 is the chain record, not an account record.
            BOOST_CHECK_EQUAL(accounts.count(0), 0U);
            Unload(std::move(wallet));
        }

        // Written: the file holds the records without the wallet being loaded.
        std::unique_ptr<WalletDatabase> database = Database(name);
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        CHDAccount record;
        BOOST_CHECK(batch->Read(std::make_pair(DBKeys::HDACCOUNT, uint32_t{1}), record));
        BOOST_CHECK_EQUAL(record.next_receive, 3U);
        CHDAccountMeta meta;
        BOOST_CHECK(batch->Read(DBKeys::HDACCOUNTMETA, meta));
    }

    std::shared_ptr<CWallet> wallet = Create("account_zero_only", walletType::bip44Wallet);
    for (uint32_t i = 0; i < 3; ++i) AddAccountKey(*wallet, 0, false, 100 + i);
    Unload(std::move(wallet));
    wallet = Load("account_zero_only");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccounts().empty());
    BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDAccountMeta());
    Unload(std::move(wallet));
}

// A wallet with HD accounts requires FEATURE_HD_ACCOUNTS, above the version new
// wallets are written with. This version loads it, refuses a wallet that
// requires more, and takes it as current: upgradewallet does not try to
// downgrade it to FEATURE_LATEST.
BOOST_AUTO_TEST_CASE(account_wallet_version)
{
    BOOST_CHECK_GT(FEATURE_HD_ACCOUNTS, FEATURE_LATEST);
    BOOST_CHECK_EQUAL(GetClosestWalletFeature(FEATURE_HD_ACCOUNTS), FEATURE_HD_ACCOUNTS);
    BOOST_CHECK_EQUAL(GetClosestWalletFeature(FEATURE_HD_ACCOUNTS - 1), FEATURE_LATEST);

    std::shared_ptr<CWallet> wallet = Create("version", walletType::bip39Wallet);
    BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_LATEST);
    wallet->SetMinVersion(FEATURE_HD_ACCOUNTS);
    Unload(std::move(wallet));

    wallet = Load("version");
    BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
    for (const int asked : {0, int{FEATURE_LATEST}, int{FEATURE_HD_ACCOUNTS}}) {
        bilingual_str error;
        BOOST_CHECK_MESSAGE(wallet->UpgradeWallet(asked, error), error.original);
        BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
    }
    bilingual_str error;
    BOOST_CHECK(!wallet->UpgradeWallet(FEATURE_HD_SPLIT, error));
    BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
    Unload(std::move(wallet));

    {
        std::unique_ptr<WalletDatabase> database = Database("version");
        BOOST_REQUIRE(WalletBatch(*database).WriteMinVersion(FEATURE_HD_ACCOUNTS + 1));
    }
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str load_error;
    std::vector<bilingual_str> warnings;
    options.require_existing = true;
    BOOST_CHECK(!LoadWallet(*m_node.chain, "version", std::nullopt, options, status, load_error, warnings));
    BOOST_CHECK(load_error.original.find("requires newer version") != std::string::npos);
}

#ifdef USE_BDB
// Salvage is of Berkeley DB files only.
// Salvage keeps what tells the keys of an account from those of account 0:
// their metadata, the account records, and the version and flag that keep a
// version without accounts from opening the wallet. The flags of the wallet
// are kept and merged with those the records require, not replaced.
BOOST_AUTO_TEST_CASE(salvage_keeps_accounts)
{
    const SecureString passphrase{"passphrase"};
    for (const bool accounts : {false, true}) {
        const std::string name{strprintf("salvage_%d", accounts)};
        std::shared_ptr<CWallet> wallet = Create(name, walletType::bip39Wallet, 0, passphrase);
        BOOST_REQUIRE(wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        BOOST_REQUIRE(wallet->Unlock(passphrase));
        CPubKey account_key;
        CHDAccount record;
        record.state = CHDAccount::State::ACTIVE;
        record.label = "salvaged";
        record.next_receive = 1;
        CHDAccountMeta meta;
        meta.highest_derived = 21;
        meta.highest_used = 1;
        if (accounts) {
            account_key = AddAccountKey(*wallet, 1, false, 0);
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            WalletBatch batch(wallet->GetDatabase());
            BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 1, record));
            BOOST_REQUIRE(spk_man.WriteHDAccountMeta(batch, meta));
        }
        BOOST_REQUIRE(wallet->Lock());
        Unload(std::move(wallet));
        uint64_t flags_before{0};
        BOOST_REQUIRE(Database(name)->MakeBatch()->Read(DBKeys::FLAGS, flags_before));
        BOOST_REQUIRE(flags_before & WALLET_FLAG_HD_SECRETS_ENCRYPTED);
        BOOST_REQUIRE(flags_before & ~uint64_t{WALLET_FLAG_HD_SECRETS_ENCRYPTED});

        bilingual_str error;
        std::vector<bilingual_str> warnings;
        BOOST_REQUIRE_MESSAGE(RecoverDatabaseFile(GetWalletDir() / name / "wallet.dat", error, warnings), error.original);

        std::unique_ptr<WalletDatabase> database = Database(name);
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        uint64_t flags{0};
        BOOST_REQUIRE(batch->Read(DBKeys::FLAGS, flags));
        BOOST_CHECK_EQUAL(flags, flags_before | (accounts ? uint64_t{WALLET_FLAG_HD_ACCOUNTS} : 0));
        int min_version{0};
        BOOST_REQUIRE(batch->Read(DBKeys::MINVERSION, min_version));
        BOOST_CHECK_EQUAL(min_version, accounts ? int{FEATURE_HD_ACCOUNTS} : int{FEATURE_LATEST});
        if (!accounts) continue;
        CHDAccount salvaged_record;
        BOOST_REQUIRE(batch->Read(std::make_pair(DBKeys::HDACCOUNT, uint32_t{1}), salvaged_record));
        BOOST_CHECK(salvaged_record == record);
        CHDAccountMeta salvaged_meta;
        BOOST_REQUIRE(batch->Read(DBKeys::HDACCOUNTMETA, salvaged_meta));
        BOOST_CHECK(salvaged_meta == meta);
        CKeyMetadata key_meta;
        BOOST_REQUIRE(batch->Read(std::make_pair(DBKeys::KEYMETA, account_key), key_meta));
        BOOST_CHECK_EQUAL(HDAccountOfKeyPath(key_meta.key_origin.path), 1U);
        CScript witprog;
        const CScript expected{GetScriptForDestination(WitnessV0KeyHash(account_key.GetID()))};
        BOOST_CHECK(batch->Read(std::make_pair(DBKeys::CSCRIPT, uint160(Hash160(expected))), witprog));
        BOOST_CHECK(witprog == expected);
    }
}
#endif // USE_BDB

// Marking a wallet as holding HD accounts sets the flag and FEATURE_HD_ACCOUNTS
// together, and derives the one account kept ahead, account 1, with its keys,
// every form of which the wallet knows as account 1's. The wallet loads as it
// was left. A wallet that cannot hold accounts is not marked.
BOOST_AUTO_TEST_CASE(setup_marks_the_wallet)
{
    for (const int wallet_type : WALLET_TYPES) {
        const std::string name{strprintf("setup_%d", wallet_type)};
        std::shared_ptr<CWallet> wallet = Create(name, wallet_type);
        BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
        BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_LATEST);
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet->GetNewDestination(OutputType::BECH32, "", dest, error));
        const CScript account_zero{GetScriptForDestination(dest)};
        BOOST_REQUIRE(Setup(*wallet));
        BOOST_REQUIRE(Setup(*wallet));

        for (int load = 0; load < 2; ++load) {
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
            BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
            const std::map<uint32_t, CHDAccount> accounts{spk_man.GetHDAccounts()};
            BOOST_REQUIRE_EQUAL(accounts.size(), 1U);
            BOOST_CHECK(accounts.at(1).state == CHDAccount::State::POOL);
            BOOST_CHECK(spk_man.GetHDAccountExtent(1) == std::make_pair(int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}, int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
            BOOST_CHECK(spk_man.GetHDAccountExtent(2) == std::make_pair(int64_t{-1}, int64_t{-1}));
            BOOST_REQUIRE(spk_man.GetHDAccountMeta());
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_used, 0U);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_derived, 1U);
            CTxDestination key_dest;
            BOOST_REQUIRE(ExtractDestination(AccountScript(*wallet, 1, true, 7), key_dest));
            CPubKey pubkey;
            BOOST_REQUIRE(spk_man.GetPubKey(ToKeyID(std::get<PKHash>(key_dest)), pubkey));
            for (const CScript& script : ScriptsOf(pubkey)) {
                BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(script), 1U);
                BOOST_CHECK_EQUAL(spk_man.IsMine(script), ISMINE_SPENDABLE);
            }
            BOOST_CHECK_EQUAL(wallet->GetHDAccountOfScript(account_zero), 0U);
            BOOST_CHECK(!spk_man.HasHDAccountsInconsistency());
            Unload(std::move(wallet));
            wallet = Load(name);
        }
        Unload(std::move(wallet));
    }

    for (const uint64_t flags : {uint64_t{WALLET_FLAG_BLANK_WALLET}, uint64_t{WALLET_FLAG_DISABLE_PRIVATE_KEYS}}) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("no_setup_%d", flags), walletType::bip32Wallet, flags);
        wallet->GetOrCreateLegacyScriptPubKeyMan();
        BOOST_CHECK(!Setup(*wallet));
        BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
        BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_LATEST);
        Unload(std::move(wallet));
    }
}

// Marking a wallet whose transaction does not commit leaves it as it was, in
// memory too: without the flag or the version, and without keys of account
// 1, so that the next attempt derives and writes them all.
BOOST_AUTO_TEST_CASE(setup_that_does_not_commit_leaves_the_wallet_as_it_was)
{
    WalletOptions walletoptions;
    walletoptions.walletType = walletType::bip32Wallet;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    g_fail_commit = false;
    std::shared_ptr<CWallet> wallet = CWallet::Create(/* chain */ nullptr, "", std::make_unique<FailCommitDatabase>(), 0, walletoptions, error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    const int version_before{wallet->GetVersion()};
    BOOST_REQUIRE_LT(version_before, int{FEATURE_HD_ACCOUNTS});

    g_fail_commit = true;
    BOOST_CHECK(!Setup(*wallet));
    g_fail_commit = false;
    BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(wallet->GetVersion(), version_before);
    BOOST_CHECK(spk_man.GetHDAccounts().empty());
    BOOST_CHECK(spk_man.GetHDAccountExtent(1) == std::make_pair(int64_t{-1}, int64_t{-1}));
    CPubKey pubkey;
    BOOST_CHECK(!spk_man.GetHDAccountPubKey(1, false, 0, pubkey));
    BOOST_CHECK(!spk_man.HasHDAccountKeys());

    BOOST_REQUIRE(Setup(*wallet));
    BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(wallet->GetVersion(), FEATURE_HD_ACCOUNTS);
    BOOST_CHECK(spk_man.GetHDAccountExtent(1) == std::make_pair(int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}, int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
}

// A payment to an account moves its next index past the key paid, and keeps
// the account after it ahead: as BIP44's account rule has it, one account
// beyond the highest used.
BOOST_AUTO_TEST_CASE(payment_keeps_the_next_account_ahead)
{
    for (const int wallet_type : WALLET_TYPES) {
        const std::string name{strprintf("ahead_%d", wallet_type)};
        std::shared_ptr<CWallet> wallet = Create(name, wallet_type);
        BOOST_REQUIRE(Setup(*wallet));
        Pay(*wallet, AccountScript(*wallet, 1, false, 5));
        Pay(*wallet, AccountScript(*wallet, 1, true, 0));

        for (int load = 0; load < 2; ++load) {
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            const std::map<uint32_t, CHDAccount> accounts{spk_man.GetHDAccounts()};
            BOOST_REQUIRE_EQUAL(accounts.size(), 2U);
            BOOST_CHECK(accounts.at(1).state == CHDAccount::State::ACTIVE);
            BOOST_CHECK_EQUAL(accounts.at(1).next_receive, 6U);
            BOOST_CHECK_EQUAL(accounts.at(1).next_change, 1U);
            BOOST_CHECK(accounts.at(2).state == CHDAccount::State::POOL);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_used, 1U);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_derived, 2U);
            // 20 keys beyond the next index on each chain.
            BOOST_CHECK(spk_man.GetHDAccountExtent(1) == std::make_pair(int64_t{6 + HD_ACCOUNT_KEY_LOOKAHEAD - 1}, int64_t{1 + HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
            BOOST_CHECK(spk_man.GetHDAccountExtent(2) == std::make_pair(int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}, int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
            BOOST_CHECK(spk_man.GetHDAccountExtent(3) == std::make_pair(int64_t{-1}, int64_t{-1}));
            Unload(std::move(wallet));
            wallet = Load(name);
        }

        // A payment to the account kept ahead keeps the next one ahead.
        Pay(*wallet, AccountScript(*wallet, 2, false, 0));
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDAccounts().at(2).state == CHDAccount::State::ACTIVE);
        BOOST_CHECK(spk_man.GetHDAccounts().at(3).state == CHDAccount::State::POOL);
        BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_used, 2U);
        // A payment to account 0 changes nothing of the accounts.
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet->GetNewDestination(OutputType::LEGACY, "", dest, error));
        Pay(*wallet, GetScriptForDestination(dest));
        BOOST_CHECK_EQUAL(spk_man.GetHDAccounts().size(), 3U);
        Unload(std::move(wallet));
    }
}

// A retired account gives no new receive address, but it is still spent from,
// and the change of each payment goes back to it: its change chain is kept
// ahead as any other account's.
BOOST_AUTO_TEST_CASE(retired_account_keeps_its_change_ahead)
{
    std::shared_ptr<CWallet> wallet = Create("retired", walletType::bip39Wallet);
    BOOST_REQUIRE(Setup(*wallet));
    Pay(*wallet, AccountScript(*wallet, 1, false, 0));
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    CHDAccount record{spk_man.GetHDAccounts().at(1)};
    record.state = CHDAccount::State::RETIRED;
    {
        WalletBatch batch(wallet->GetDatabase());
        BOOST_REQUIRE(spk_man.WriteHDAccount(batch, 1, record));
    }
    const int64_t receive_extent{spk_man.GetHDAccountExtent(1).first};
    for (uint32_t index = 0; index < 2 * HD_ACCOUNT_KEY_LOOKAHEAD; ++index) {
        CPubKey pubkey;
        BOOST_REQUIRE_MESSAGE(spk_man.GetHDAccountPubKey(1, true, index, pubkey), strprintf("change key %u", index));
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return spk_man.HandOutHDAccountKey(1, true, index)));
    }
    BOOST_CHECK_EQUAL(spk_man.GetHDAccountExtent(1).second, int64_t{3 * HD_ACCOUNT_KEY_LOOKAHEAD - 1});
    BOOST_CHECK_EQUAL(spk_man.GetHDAccountExtent(1).first, receive_extent);
    Unload(std::move(wallet));
}

// The next account cannot be derived while the wallet is locked: a payment to
// an account is noted, and the look-ahead is owed. The wallet's worker derives
// it once the wallet is unlocked, fully or for staking only, and does nothing
// while it is locked.
BOOST_AUTO_TEST_CASE(locked_wallet_owes_the_lookahead)
{
    const SecureString passphrase{"passphrase"};
    for (const bool staking_only : {false, true}) {
        for (const int wallet_type : WALLET_TYPES) {
            std::shared_ptr<CWallet> wallet = Create(strprintf("owed_%d_%d", staking_only, wallet_type), wallet_type, 0, passphrase);
            BOOST_REQUIRE(wallet->Unlock(passphrase));
            BOOST_REQUIRE(Setup(*wallet));
            const CScript paid{AccountScript(*wallet, 1, false, 3)};
            BOOST_REQUIRE(wallet->Lock());
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());

            Pay(*wallet, paid);
            BOOST_CHECK(spk_man.GetHDAccounts().at(1).state == CHDAccount::State::ACTIVE);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccounts().at(1).next_receive, 4U);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccountMeta()->highest_used, 1U);
            BOOST_CHECK_EQUAL(spk_man.GetHDAccounts().count(2), 0U);
            BOOST_CHECK(spk_man.IsHDAccountsTopUpOwed());

            wallet->ScheduleHDAccountWork();
            wallet->SyncWithHDAccountWorker();
            BOOST_CHECK_EQUAL(spk_man.GetHDAccounts().count(2), 0U);

            BOOST_REQUIRE(wallet->Unlock(passphrase));
            if (staking_only) wallet->SetIsWalletStakingOnly(true);
            wallet->SyncWithHDAccountWorker();
            BOOST_CHECK(spk_man.GetHDAccounts().at(2).state == CHDAccount::State::POOL);
            BOOST_CHECK(spk_man.GetHDAccountExtent(2) == std::make_pair(int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}, int64_t{HD_ACCOUNT_KEY_LOOKAHEAD - 1}));
            BOOST_CHECK(spk_man.GetHDAccountExtent(1).first == int64_t{4 + HD_ACCOUNT_KEY_LOOKAHEAD - 1});
            BOOST_CHECK(!spk_man.IsHDAccountsTopUpOwed());
            if (staking_only) wallet->SetIsWalletStakingOnly(false);
            BOOST_REQUIRE(wallet->Lock());
            // The worker is joined when the wallet goes.
            Unload(std::move(wallet));
        }
    }
}

// A locked wallet that sees a payment whose look-ahead it already holds, as a
// wallet loaded with its best block behind the chain sees its payments again,
// owes nothing. A payment that needs keys it does not hold is still owed.
BOOST_AUTO_TEST_CASE(locked_wallet_owes_nothing_it_holds)
{
    const SecureString passphrase{"passphrase"};
    for (const int wallet_type : WALLET_TYPES) {
        std::shared_ptr<CWallet> wallet = Create(strprintf("held_%d", wallet_type), wallet_type, 0, passphrase);
        BOOST_REQUIRE(wallet->Unlock(passphrase));
        BOOST_REQUIRE(Setup(*wallet));
        const CScript paid{AccountScript(*wallet, 1, false, 3)};
        Pay(*wallet, paid);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(!spk_man.IsHDAccountsTopUpOwed());
        BOOST_REQUIRE(spk_man.GetHDAccounts().count(2));
        BOOST_REQUIRE(wallet->Lock());

        // The same address paid again: its keys and the account kept ahead are there.
        Pay(*wallet, paid);
        BOOST_CHECK(!spk_man.IsHDAccountsTopUpOwed());

        // The account kept ahead paid: the account after it is owed.
        Pay(*wallet, AccountScript(*wallet, 2, false, 0));
        BOOST_CHECK(spk_man.IsHDAccountsTopUpOwed());
        Unload(std::move(wallet));
    }
}

// A wallet that holds keys of an HD account above 0 without the flag, as an
// earlier wallet tool's salvage leaves it, is loaded and known to be so.
BOOST_AUTO_TEST_CASE(account_keys_without_the_flag_are_inconsistent)
{
    std::shared_ptr<CWallet> wallet = Create("unflagged", walletType::bip39Wallet);
    AddAccountKey(*wallet, 1, false, 0);
    Unload(std::move(wallet));
    wallet = Load("unflagged");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->HasHDAccountsInconsistency());
    Unload(std::move(wallet));

    wallet = Create("flagged", walletType::bip39Wallet);
    BOOST_REQUIRE(Setup(*wallet));
    Unload(std::move(wallet));
    wallet = Load("flagged");
    BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->HasHDAccountsInconsistency());
    Unload(std::move(wallet));
}

// A wallet that holds HD accounts keeps its seed when it is encrypted, a plain
// HD (bip32) wallet included, which is given a new seed otherwise; and a new
// seed cannot be forced on it. The seed is compared through the keys it
// derives: the seed id of every wallet made from a mnemonic is the same.
BOOST_AUTO_TEST_CASE(account_wallet_keeps_its_seed)
{
    const SecureString passphrase{"passphrase"};
    for (const bool accounts : {false, true}) {
        for (const int wallet_type : WALLET_TYPES) {
            std::shared_ptr<CWallet> wallet = Create(strprintf("keeps_%d_%d", accounts, wallet_type), wallet_type);
            LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
            if (accounts) BOOST_REQUIRE(Setup(*wallet));
            const auto key_of = [&](uint32_t account) {
                CKey key;
                CKeyMetadata meta;
                BOOST_REQUIRE(WITH_LOCK(spk_man.cs_KeyStore, return spk_man.DeriveHDAccountKey(account, false, 0, key, meta)));
                return key.GetPubKey();
            };
            const CPubKey zero_before{key_of(0)};
            const CPubKey one_before{key_of(1)};

            BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
            BOOST_REQUIRE(wallet->Unlock(passphrase));
            const bool keeps{accounts || wallet_type != walletType::bip32Wallet};
            BOOST_CHECK_EQUAL(key_of(0) == zero_before, keeps);
            if (accounts) {
                BOOST_CHECK(key_of(1) == one_before);
                CPubKey account_key;
                BOOST_CHECK(spk_man.GetHDAccountPubKey(1, false, 0, account_key) && account_key == one_before);

                // A new seed forced on it, as encryption would have done, is refused.
                WalletOptions walletoptions;
                walletoptions.walletType = wallet_type;
                BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return spk_man.SetupGeneration(walletoptions, /* force */ true)));
                BOOST_CHECK(key_of(0) == zero_before);
                BOOST_CHECK(key_of(1) == one_before);
            }
            BOOST_REQUIRE(wallet->Lock());
            Unload(std::move(wallet));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
