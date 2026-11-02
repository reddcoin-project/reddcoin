// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <script/standard.h>
#include <streams.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>
#include <validationinterface.h>
#include <wallet/db.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <fstream>
#include <iterator>
#include <thread>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
const SecureString PHRASE{"abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"};
const SecureString PHRASE_PASSPHRASE{"a second factor for the mnemonic 0451"};
const SecureString WALLET_PASSPHRASE{"the passphrase the wallet is encrypted with"};

//! A chain, and wallets in files, created from a known mnemonic.
struct HDChainTestingSetup : public TestChain100Setup {
    HDChainTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
    }

    std::shared_ptr<CWallet> Create(const std::string& name, int wallet_type, const SecureString& phrase_passphrase = "", const SecureString& wallet_passphrase = "", DatabaseFormat format = DatabaseFormat::BERKELEY)
    {
        DatabaseOptions options;
        options.require_format = format;
        DatabaseStatus status;
        WalletOptions walletoptions;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        options.create_passphrase = wallet_passphrase;
        walletoptions.walletType = wallet_type;
        if (wallet_type != walletType::bip32Wallet) {
            walletoptions.ssMnemonic = PHRASE;
            walletoptions.ssMnemonicPassphrase = phrase_passphrase;
        }
        std::shared_ptr<CWallet> wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }

    //! Load a wallet. Most wallets here load without a word; one in a state its owner has to be told about does not.
    std::shared_ptr<CWallet> Load(const std::string& name, bool expect_warnings = false)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet = LoadWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK_EQUAL(warnings.empty(), !expect_warnings);
        return wallet;
    }

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
    }

    //! Change the records of a wallet that is not loaded.
    void Edit(const std::string& name, const std::function<void(DatabaseBatch&)>& edit)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        options.require_existing = true;
        std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
        BOOST_REQUIRE_MESSAGE(database, error.original);
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        edit(*batch);
    }

    //! The chain record of an unencrypted wallet of PHRASE and PHRASE_PASSPHRASE: the secrets in clear.
    CHDChain ClearChain(const std::string& name)
    {
        Unload(Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE));
        CHDChain chain;
        Edit(name, [&](DatabaseBatch& batch) { BOOST_REQUIRE(batch.Read(DBKeys::HDCHAIN, chain)); });
        BOOST_REQUIRE(!chain.vchSeed.empty() && !chain.vchMnemonic.empty());
        return chain;
    }

    static void SetFlag(DatabaseBatch& batch, bool set)
    {
        uint64_t flags{0};
        batch.Read(DBKeys::FLAGS, flags);
        flags = set ? flags | WALLET_FLAG_HD_SECRETS_ENCRYPTED : flags & ~uint64_t{WALLET_FLAG_HD_SECRETS_ENCRYPTED};
        BOOST_REQUIRE(batch.Write(DBKeys::FLAGS, flags));
    }

    //! An encrypted wallet as versions before this one left it: the secrets in the chain record, no record of their own, no flag.
    void MakeUnmigrated(const std::string& name, const CHDChain& clear)
    {
        Unload(Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
        Edit(name, [&](DatabaseBatch& batch) {
            BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, clear));
            BOOST_REQUIRE(batch.Erase(DBKeys::HDSECRETS));
            SetFlag(batch, false);
        });
    }

    //! A wallet that cannot derive goes on working with what it has, and nothing throws.
    void CheckWorksWithoutSeed(CWallet& wallet)
    {
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDSecretsState() == LegacyScriptPubKeyMan::HDSecretsState::UNREADABLE);
        if (wallet.IsCrypted()) BOOST_REQUIRE(wallet.Unlock(WALLET_PASSPHRASE));
        BOOST_CHECK(spk_man.GetHDSecretsState() == LegacyScriptPubKeyMan::HDSecretsState::UNREADABLE);
        const size_t keys_before = spk_man.GetKeys().size();
        // The pool cannot be filled, and what is in it is handed out.
        BOOST_CHECK(!WITH_LOCK(wallet.cs_wallet, return spk_man.TopUp()));
        const unsigned int pool = WITH_LOCK(wallet.cs_wallet, return wallet.GetKeyPoolSize());
        BOOST_CHECK_GT(pool, 0U);
        std::vector<CScript> scripts;
        for (unsigned int i = 0; i < pool + 4; ++i) {
            CTxDestination dest;
            std::string error;
            if (wallet.GetNewDestination(OutputType::LEGACY, "", dest, error)) scripts.push_back(GetScriptForDestination(dest));
        }
        BOOST_CHECK_GT(scripts.size(), 0U);
        BOOST_CHECK_LE(scripts.size(), pool);
        // A payment to a key that was in the pool: a wallet that could derive would top up here.
        BOOST_CHECK_NO_THROW(WITH_LOCK(wallet.cs_wallet, spk_man.MarkUnusedAddresses(scripts.back())));
        BOOST_CHECK_EQUAL(spk_man.GetKeys().size(), keys_before);
        BOOST_CHECK(!wallet.GetHDWarnings().empty());
    }

    //! The next addresses of the wallet.
    std::vector<std::string> Addresses(CWallet& wallet, int count)
    {
        std::vector<std::string> addresses;
        for (int i = 0; i < count; ++i) {
            CTxDestination dest;
            std::string error;
            BOOST_REQUIRE_MESSAGE(wallet.GetNewDestination(OutputType::LEGACY, "", dest, error), error);
            addresses.push_back(EncodeDestination(dest));
        }
        return addresses;
    }

    //! The types of the records of the wallet's file whose value holds the bytes.
    std::vector<std::string> RecordsHolding(CWallet& wallet, const std::vector<unsigned char>& bytes)
    {
        std::vector<std::string> types;
        std::unique_ptr<DatabaseBatch> batch = wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(batch->StartCursor());
        while (true) {
            CDataStream key(SER_DISK, CLIENT_VERSION);
            CDataStream value(SER_DISK, CLIENT_VERSION);
            bool complete;
            if (!batch->ReadAtCursor(key, value, complete) || complete) break;
            if (std::search(value.begin(), value.end(), bytes.begin(), bytes.end()) != value.end()) {
                std::string type;
                key >> type;
                types.push_back(type);
            }
        }
        batch->CloseCursor();
        return types;
    }

    //! The secrets of the wallet made from PHRASE, as they would stand in a file.
    struct Secrets {
        std::vector<unsigned char> phrase;
        std::vector<unsigned char> phrase_passphrase;
        std::vector<unsigned char> seed;
    };
    Secrets SecretsOf(const SecureString& phrase_passphrase)
    {
        CHDChain chain;
        SecureVector seed;
        BOOST_REQUIRE(chain.SetMnemonic(PHRASE, phrase_passphrase, seed));
        return {{PHRASE.begin(), PHRASE.end()}, {phrase_passphrase.begin(), phrase_passphrase.end()}, {seed.begin(), seed.end()}};
    }

    //! The files under the wallet's directory that hold the bytes, as they are on disk now.
    std::vector<std::string> FilesHolding(const CWallet& wallet, const std::vector<unsigned char>& bytes)
    {
        std::vector<std::string> files;
        const fs::path directory = GetWalletDir() / wallet.GetName();
        for (fs::recursive_directory_iterator it(directory), end; it != end; ++it) {
            if (!fs::is_regular_file(it->path())) continue;
            if (it->path().filename() == ".walletlock") continue; // empty, and locked while its wallet is loaded: Windows refuses to read it
            std::ifstream file(it->path().string(), std::ios::binary);
            const std::vector<unsigned char> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (std::search(content.begin(), content.end(), bytes.begin(), bytes.end()) != content.end()) {
                files.push_back(fs::relative(it->path(), directory).string());
            }
        }
        return files;
    }

    void CheckNoneInFile(CWallet& wallet, const Secrets& secrets)
    {
        // Nothing in any file of the wallet's directory, the database's own
        // log or journal included, while the wallet is loaded. This process
        // has the secrets in its memory and none of that may be written.
        for (const auto& [what, bytes] : {std::make_pair("phrase", secrets.phrase), std::make_pair("seed", secrets.seed), std::make_pair("mnemonic passphrase", secrets.phrase_passphrase)}) {
            if (bytes.empty()) continue;
            const std::vector<std::string> files = FilesHolding(wallet, bytes);
            BOOST_CHECK_MESSAGE(files.empty(), strprintf("the %s is in the file %s", what, files.empty() ? "" : files[0]));
        }
        BOOST_CHECK_MESSAGE(RecordsHolding(wallet, secrets.phrase).empty(), "the phrase is in clear in the file");
        BOOST_CHECK_MESSAGE(RecordsHolding(wallet, secrets.seed).empty(), "the seed is in clear in the file");
        if (!secrets.phrase_passphrase.empty()) {
            BOOST_CHECK_MESSAGE(RecordsHolding(wallet, secrets.phrase_passphrase).empty(), "the mnemonic passphrase is in clear in the file");
        }
    }

    void CheckNoneInMemory(CWallet& wallet)
    {
        const CHDChain& chain = Assert(wallet.GetLegacyScriptPubKeyMan())->GetHDChain();
        BOOST_CHECK_MESSAGE(chain.vchMnemonic.empty(), "the phrase is held in memory while locked");
        BOOST_CHECK_MESSAGE(chain.vchMnemonicPassphrase.empty(), "the mnemonic passphrase is held in memory while locked");
        BOOST_CHECK_MESSAGE(chain.vchSeed.empty(), "the seed is held in memory while locked");
        // And nothing gives them out: neither the seed for derivation nor the secrets for the commands that print them.
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        SecureVector seed;
        CHDSecrets secrets;
        BOOST_CHECK(!spk_man.GetHDSeed(chain, seed));
        BOOST_CHECK(!spk_man.GetHDSecrets(secrets));
        BOOST_CHECK(spk_man.GetHDSecretsState() == LegacyScriptPubKeyMan::HDSecretsState::ENCRYPTED);
        BOOST_CHECK(wallet.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdchain_tests, HDChainTestingSetup)

// What the records of an unencrypted wallet hold is the reference: the phrase,
// its passphrase and the seed are in the hdchain record, and nowhere else.
BOOST_AUTO_TEST_CASE(unencrypted_mnemonic_wallet_keeps_its_chain_record)
{
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    std::shared_ptr<CWallet> wallet = Create("clear", walletType::bip44Wallet, PHRASE_PASSPHRASE);
    const std::vector<std::string> only_chain{"hdchain"};
    BOOST_CHECK(RecordsHolding(*wallet, secrets.phrase) == only_chain);
    BOOST_CHECK(RecordsHolding(*wallet, secrets.phrase_passphrase) == only_chain);
    BOOST_CHECK(RecordsHolding(*wallet, secrets.seed) == only_chain);
    BOOST_CHECK(!wallet->IsCrypted());
    // Not flagged, no record of its own for the secrets, and they are where they were.
    BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == LegacyScriptPubKeyMan::HDSecretsState::IN_CHAIN);
    {
        std::unique_ptr<DatabaseBatch> batch = wallet->GetDatabase().MakeBatch();
        BOOST_CHECK(!batch->Exists(DBKeys::HDSECRETS));
    }
    Unload(std::move(wallet));
}

// A wallet created from a mnemonic and encrypted, when it is created or
// afterwards, has no record that holds the phrase, the mnemonic passphrase
// or the seed in clear, and holds none of them in memory while it is locked.
BOOST_AUTO_TEST_CASE(encrypted_mnemonic_wallet_keeps_no_clear_secret)
{
    for (const DatabaseFormat format : {DatabaseFormat::BERKELEY, DatabaseFormat::SQLITE}) {
    for (const int wallet_type : {walletType::bip39Wallet, walletType::bip44Wallet}) {
        for (const SecureString& phrase_passphrase : {SecureString{}, PHRASE_PASSPHRASE}) {
            for (const bool at_creation : {true, false}) {
                const std::string name{strprintf("enc_%d_%d_%d_%d", wallet_type, !phrase_passphrase.empty(), at_creation, format == DatabaseFormat::SQLITE)};
                BOOST_TEST_MESSAGE(name);
                const Secrets secrets = SecretsOf(phrase_passphrase);
                std::shared_ptr<CWallet> wallet = Create(name, wallet_type, phrase_passphrase, at_creation ? WALLET_PASSPHRASE : SecureString{}, format);
                BOOST_CHECK_EQUAL(wallet->GetDatabase().Format(), format == DatabaseFormat::SQLITE ? "sqlite" : "bdb");
                if (!at_creation) BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
                BOOST_REQUIRE(wallet->IsCrypted());
                if (!wallet->IsLocked()) BOOST_REQUIRE(wallet->Lock());
                CheckNoneInFile(*wallet, secrets);
                CheckNoneInMemory(*wallet);

                // The same after it has been closed and loaded.
                Unload(std::move(wallet));
                wallet = Load(name);
                BOOST_CHECK(wallet->IsLocked());
                CheckNoneInFile(*wallet, secrets);
                CheckNoneInMemory(*wallet);
                // It is still known to be a wallet of its kind.
                LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
                BOOST_CHECK(spk_man.IsBip39Enabled());
                BOOST_CHECK_EQUAL(spk_man.IsBip44Enabled(), wallet_type == walletType::bip44Wallet);

                // And after it has been unlocked, used and locked again.
                BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
                {
                    // Unlocked, the secrets are there, and they are the ones the wallet was made from.
                    CHDSecrets unlocked;
                    BOOST_REQUIRE(spk_man.GetHDSecrets(unlocked));
                    BOOST_CHECK(std::vector<unsigned char>(unlocked.vchMnemonic.begin(), unlocked.vchMnemonic.end()) == secrets.phrase);
                    BOOST_CHECK(std::vector<unsigned char>(unlocked.vchMnemonicPassphrase.begin(), unlocked.vchMnemonicPassphrase.end()) == secrets.phrase_passphrase);
                    BOOST_CHECK(std::vector<unsigned char>(unlocked.vchSeed.begin(), unlocked.vchSeed.end()) == secrets.seed);
                    // The chain object still holds none of them.
                    BOOST_CHECK(spk_man.GetHDChain().vchSeed.empty() && spk_man.GetHDChain().vchMnemonic.empty());
                }
                Addresses(*wallet, 6);
                BOOST_REQUIRE(wallet->Lock());
                CheckNoneInFile(*wallet, secrets);
                CheckNoneInMemory(*wallet);
                Unload(std::move(wallet));
            }
        }
    }
    }
}

// Encrypting changes where the secrets are kept, not what is derived from
// them: the addresses of an encrypted wallet are those of an unencrypted
// wallet of the same phrase, past the keys held ready when it was encrypted.
BOOST_AUTO_TEST_CASE(encrypted_mnemonic_wallet_derives_the_same_keys)
{
    for (const int wallet_type : {walletType::bip39Wallet, walletType::bip44Wallet}) {
        std::shared_ptr<CWallet> clear = Create(strprintf("same_clear_%d", wallet_type), wallet_type, PHRASE_PASSPHRASE);
        const std::vector<std::string> expected = Addresses(*clear, 12);
        Unload(std::move(clear));

        for (const bool at_creation : {true, false}) {
            const std::string name{strprintf("same_enc_%d_%d", wallet_type, at_creation)};
            std::shared_ptr<CWallet> wallet = Create(name, wallet_type, PHRASE_PASSPHRASE, at_creation ? WALLET_PASSPHRASE : SecureString{});
            if (!at_creation) BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
            Unload(std::move(wallet));
            wallet = Load(name);
            BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
            BOOST_CHECK(Addresses(*wallet, 12) == expected);
            Unload(std::move(wallet));
        }
    }
}

// The note that the file still has to be rewritten, left behind when a node
// stopped between moving the seed and rewriting: the next load rewrites and
// removes it, with the wallet locked.
BOOST_AUTO_TEST_CASE(pending_rewrite_is_done_at_load)
{
    for (const DatabaseFormat format : {DatabaseFormat::BERKELEY, DatabaseFormat::SQLITE}) {
        const std::string name{strprintf("pending_%d", format == DatabaseFormat::SQLITE)};
        std::shared_ptr<CWallet> wallet = Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE, format);
        BOOST_REQUIRE(WalletBatch(wallet->GetDatabase()).WriteHDSecretsPending());
        Unload(std::move(wallet));

        wallet = Load(name);
        BOOST_CHECK(wallet->IsLocked());
        BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->IsHDSecretsRewritePending());
        BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
        // The database works after it: something is written and read back.
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        Addresses(*wallet, 5);
        Unload(std::move(wallet));
        wallet = Load(name);
        BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->IsHDSecretsRewritePending());
        Unload(std::move(wallet));
    }
}

// A rewrite of the database file holds the wallet lock and waits until no
// batch is open. An address book change that came at that moment used to open
// its batch first and then wait for the lock: neither ever finished.
BOOST_AUTO_TEST_CASE(address_book_change_does_not_block_a_rewrite)
{
    std::shared_ptr<CWallet> wallet = Create("rewrite_and_book", walletType::bip32Wallet);
    CKey key;
    key.MakeNewKey(true);
    const CTxDestination other{PKHash(key.GetPubKey())};
    BOOST_REQUIRE(wallet->SetAddressBook(other, "a label", "send"));

    std::thread change;
    {
        LOCK(wallet->cs_wallet);
        change = std::thread([&] { wallet->DelAddressBook(other); });
        // Time for the other thread to reach the lock.
        UninterruptibleSleep(std::chrono::milliseconds{300});
        BOOST_CHECK(wallet->GetDatabase().Rewrite());
    }
    change.join();
    BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->FindAddressBookEntry(other) == nullptr));
    Unload(std::move(wallet));
}

using HDState = LegacyScriptPubKeyMan::HDSecretsState;

// Where the secrets are is decided from four things that are read in no
// particular order: the flag, the record, the chain and the master key. One
// case for each combination that can be met.
BOOST_AUTO_TEST_CASE(state_decided_when_loaded)
{
    const CHDChain clear = ClearChain("cases_clear");
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);

    // F: not encrypted. As it always was.
    std::shared_ptr<CWallet> wallet = Load("cases_clear");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == HDState::IN_CHAIN);
    BOOST_CHECK(wallet->GetHDWarnings().empty());
    Unload(std::move(wallet));

    // I: a seed that is a key of the wallet.
    Unload(Create("cases_plain", walletType::bip32Wallet, "", WALLET_PASSPHRASE));
    wallet = Load("cases_plain");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == HDState::NOT_HELD);
    BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETS));
    Unload(std::move(wallet));

    // A: encrypted by this version.
    Unload(Create("cases_a", walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
    wallet = Load("cases_a");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_CHECK(wallet->GetHDWarnings().empty());
    Unload(std::move(wallet));

    // B, B2, E: the secrets are in the chain record of an encrypted wallet,
    // whatever else is there. All are moved at the first unlock.
    struct Unmigrated { const char* name; bool flag; bool record; };
    for (const Unmigrated& which : {Unmigrated{"cases_b", false, false}, Unmigrated{"cases_b2", true, false}, Unmigrated{"cases_e", true, true}, Unmigrated{"cases_e2", false, true}}) {
        BOOST_TEST_MESSAGE(which.name);
        Unload(Create(which.name, walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
        CHDSecretsRecord old_record;
        Edit(which.name, [&](DatabaseBatch& batch) {
            BOOST_REQUIRE(batch.Read(DBKeys::HDSECRETS, old_record));
            BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, clear));
            if (!which.record) BOOST_REQUIRE(batch.Erase(DBKeys::HDSECRETS));
            SetFlag(batch, which.flag);
        });
        wallet = Load(which.name, /* expect_warnings */ true);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::UNMIGRATED);
        BOOST_CHECK(spk_man.IsBip44Enabled());
        BOOST_CHECK_EQUAL(wallet->GetHDWarnings().size(), 1U);
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
        BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        BOOST_CHECK(spk_man.IsBip44Enabled());
        BOOST_CHECK(!spk_man.IsHDSecretsRewritePending());
        BOOST_CHECK(wallet->GetHDWarnings().empty());
        Addresses(*wallet, 5);
        BOOST_REQUIRE(wallet->Lock());
        CheckNoneInFile(*wallet, secrets);
        CheckNoneInMemory(*wallet);
        // The record was written anew, with an IV of its own.
        CHDSecretsRecord new_record;
        BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Read(DBKeys::HDSECRETS, new_record));
        BOOST_CHECK(new_record.nIV != old_record.nIV);
        BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
        Unload(std::move(wallet));
    }

    // D: the record without its flag, as the salvage of an earlier wallet tool leaves it. The flag is put back.
    Unload(Create("cases_d", walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
    Edit("cases_d", [&](DatabaseBatch& batch) { SetFlag(batch, false); });
    wallet = Load("cases_d");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->IsBip44Enabled());
    {
        uint64_t flags{0};
        BOOST_REQUIRE(wallet->GetDatabase().MakeBatch()->Read(DBKeys::FLAGS, flags));
        BOOST_CHECK(flags & WALLET_FLAG_HD_SECRETS_ENCRYPTED);
    }
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    Addresses(*wallet, 5);
    Unload(std::move(wallet));

    // C, G: an encrypted wallet whose chain has no seed and that has no readable record of it.
    struct Unreadable { const char* name; bool flag; int record; }; // record: 0 absent, 1 another version, 2 does not decrypt
    for (const Unreadable& which : {Unreadable{"cases_c", true, 0}, Unreadable{"cases_c_version", true, 1}, Unreadable{"cases_g", false, 0}}) {
        BOOST_TEST_MESSAGE(which.name);
        Unload(Create(which.name, walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
        Edit(which.name, [&](DatabaseBatch& batch) {
            CHDSecretsRecord record;
            BOOST_REQUIRE(batch.Read(DBKeys::HDSECRETS, record));
            if (which.record == 0) BOOST_REQUIRE(batch.Erase(DBKeys::HDSECRETS));
            if (which.record == 1) {
                record.nVersion = 2;
                BOOST_REQUIRE(batch.Write(DBKeys::HDSECRETS, record));
            }
            SetFlag(batch, which.flag);
        });
        wallet = Load(which.name, /* expect_warnings */ true);
        CheckWorksWithoutSeed(*wallet);
        Unload(std::move(wallet));
    }

    // C, found only when it is unlocked: a record that does not decrypt. The unlock succeeds.
    Unload(Create("cases_c_unlock", walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
    Edit("cases_c_unlock", [&](DatabaseBatch& batch) {
        CHDSecretsRecord record;
        BOOST_REQUIRE(batch.Read(DBKeys::HDSECRETS, record));
        record.vchCrypted[3] ^= 1;
        BOOST_REQUIRE(batch.Write(DBKeys::HDSECRETS, record));
    });
    wallet = Load("cases_c_unlock");
    BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    CheckWorksWithoutSeed(*wallet);
    Unload(std::move(wallet));

    // H: the flag on a wallet that is not encrypted and has its secrets in the
    // chain. They stay there: deriving a key must not write the chain without them.
    Unload(Create("cases_h", walletType::bip44Wallet, PHRASE_PASSPHRASE));
    Edit("cases_h", [&](DatabaseBatch& batch) { SetFlag(batch, true); });
    for (int round = 0; round < 2; ++round) {
        wallet = Load("cases_h");
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::IN_CHAIN);
        CHDSecrets held;
        BOOST_REQUIRE(spk_man.GetHDSecrets(held));
        BOOST_CHECK(std::vector<unsigned char>(held.vchSeed.begin(), held.vchSeed.end()) == secrets.seed);
        Addresses(*wallet, 5);
        Unload(std::move(wallet));
    }

    // H2 and the unencrypted G: no seed in the chain of a wallet that is not encrypted.
    for (const bool with_record : {true, false}) {
        const std::string name{with_record ? "cases_h2" : "cases_g_clear"};
        Unload(Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE));
        Edit(name, [&](DatabaseBatch& batch) {
            CHDChain chain{clear};
            SecureVector().swap(chain.vchMnemonic);
            SecureVector().swap(chain.vchMnemonicPassphrase);
            SecureVector().swap(chain.vchSeed);
            BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, chain));
            if (with_record) BOOST_REQUIRE(batch.Write(DBKeys::HDSECRETS, CHDSecretsRecord{}));
        });
        wallet = Load(name, /* expect_warnings */ true);
        CheckWorksWithoutSeed(*wallet);
        Unload(std::move(wallet));
    }
}

// The move of the secrets is one transaction: when any of its writes fails,
// nothing has changed, in the file or in memory, and the wallet is unlocked as
// it was. Until the move succeeds the chain record is the only copy of the
// seed, and a new key must not write it without the seed.
BOOST_AUTO_TEST_CASE(failed_move_changes_nothing)
{
    const CHDChain clear = ClearChain("fail_clear");
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    MakeUnmigrated("fail", clear);

    for (const std::string& failing : {"txnbegin", "flags", "hdsecrets", "hdchain", "hdsecretspending", "txncommit"}) {
        BOOST_TEST_MESSAGE(failing);
        std::shared_ptr<CWallet> wallet = Load("fail", /* expect_warnings */ true);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        g_wallet_hd_write_fault = [&](const std::string& what) { return what == failing; };
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        g_wallet_hd_write_fault = nullptr;
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::UNMIGRATED);
        BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        BOOST_CHECK(spk_man.IsBip44Enabled());
        BOOST_CHECK(!spk_man.IsHDSecretsRewritePending());
        BOOST_CHECK(!spk_man.GetHDChain().vchSeed.empty());
        {
            std::unique_ptr<DatabaseBatch> batch = wallet->GetDatabase().MakeBatch();
            BOOST_CHECK(!batch->Exists(DBKeys::HDSECRETS));
            BOOST_CHECK(!batch->Exists(DBKeys::HDSECRETSPENDING));
            uint64_t flags{0};
            batch->Read(DBKeys::FLAGS, flags);
            BOOST_CHECK(!(flags & WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        }
        // It is unlocked and derives; the chain record written for the new keys still has the seed.
        Addresses(*wallet, 5);
        Unload(std::move(wallet));
        CHDChain on_disk;
        Edit("fail", [&](DatabaseBatch& batch) { BOOST_REQUIRE(batch.Read(DBKeys::HDCHAIN, on_disk)); });
        BOOST_CHECK(std::vector<unsigned char>(on_disk.vchSeed.begin(), on_disk.vchSeed.end()) == secrets.seed);
        BOOST_CHECK(!on_disk.vchMnemonic.empty());
    }

    // The rewrite fails: the wallet is moved, the note stays, and it is said. The next unlock finishes it.
    std::shared_ptr<CWallet> wallet = Load("fail", /* expect_warnings */ true);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    g_wallet_hd_write_fault = [](const std::string& what) { return what == "rewrite"; };
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    g_wallet_hd_write_fault = nullptr;
    BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(spk_man.IsHDSecretsRewritePending());
    BOOST_CHECK(wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
    BOOST_CHECK_EQUAL(wallet->GetHDWarnings().size(), 1U);
    BOOST_REQUIRE(wallet->Lock());
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    BOOST_CHECK(!spk_man.IsHDSecretsRewritePending());
    BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
    BOOST_CHECK(wallet->GetHDWarnings().empty());
    BOOST_REQUIRE(wallet->Lock());
    CheckNoneInFile(*wallet, secrets);
    Unload(std::move(wallet));
}

// A chain made from a seed given outright has a seed and no phrase. Unflagged
// it reads as a wallet with a plain seed, as it always did. Encrypting it
// keeps its seed, which is then in the encrypted record, and from then on it
// is of the kind whose seed must not be replaced.
BOOST_AUTO_TEST_CASE(seed_without_phrase_is_encrypted_and_kept)
{
    const CHDChain clear = ClearChain("seedonly_source");
    for (const size_t seed_size : {size_t{64}, size_t{32}}) {
        const std::string name{strprintf("seedonly_%d", seed_size)};
        Unload(Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE));
        CHDChain chain{clear};
        SecureVector().swap(chain.vchMnemonic);
        SecureVector().swap(chain.vchMnemonicPassphrase);
        chain.vchSeed.resize(seed_size);
        const SecureVector seed = chain.vchSeed;
        Edit(name, [&](DatabaseBatch& batch) { BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, chain)); });

        std::shared_ptr<CWallet> wallet = Load(name);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::IN_CHAIN);
        BOOST_CHECK(spk_man.IsHDEnabled() && !spk_man.IsBip39Enabled());
        const CKeyID seed_id = spk_man.GetHDChain().seed_id;
        const std::vector<std::string> before = Addresses(*wallet, 2);

        BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
        BOOST_CHECK(spk_man.GetHDChain().seed_id == seed_id);
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
        BOOST_CHECK(wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
        BOOST_CHECK(spk_man.IsBip39Enabled());
        BOOST_CHECK(spk_man.GetHDChain().vchSeed.empty());
        Unload(std::move(wallet));

        wallet = Load(name);
        LegacyScriptPubKeyMan& reloaded = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(reloaded.IsBip39Enabled());
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        CHDSecrets held;
        BOOST_REQUIRE(reloaded.GetHDSecrets(held));
        BOOST_CHECK(held.vchSeed == seed);
        BOOST_CHECK(held.vchMnemonic.empty() && held.vchMnemonicPassphrase.empty());
        Addresses(*wallet, 6);
        Unload(std::move(wallet));
    }
}

// encryptwallet moves the secrets too: the rewrite that follows is owed like
// the one after a move at unlock. Done, the note is gone; failed, it stays and
// the next unlock finishes it.
BOOST_AUTO_TEST_CASE(rewrite_owed_after_encryptwallet)
{
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    for (const bool rewrite_fails : {false, true}) {
        const std::string name{strprintf("encrypt_rewrite_%d", rewrite_fails)};
        std::shared_ptr<CWallet> wallet = Create(name, walletType::bip44Wallet, PHRASE_PASSPHRASE);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        int rewrites{0};
        g_wallet_hd_write_fault = [&](const std::string& what) {
            if (what != "rewrite") return false;
            ++rewrites;
            return rewrite_fails;
        };
        BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
        g_wallet_hd_write_fault = nullptr;
        // Once when it succeeds (at the unlock encryptwallet does), and tried again once when it fails.
        BOOST_CHECK_EQUAL(rewrites, rewrite_fails ? 2 : 1);
        // encryptwallet gives its own answer: nothing is left for the GUI to show at the next unlock.
        BOOST_CHECK(wallet->TakeHDSecretsMovedNotices().empty());
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
        BOOST_CHECK_EQUAL(spk_man.IsHDSecretsRewritePending(), rewrite_fails);
        BOOST_CHECK_EQUAL(wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING), rewrite_fails);
        BOOST_CHECK_EQUAL(wallet->GetHDWarnings().size(), rewrite_fails ? 1U : 0U);
        if (rewrite_fails) {
            BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
            BOOST_CHECK(!spk_man.IsHDSecretsRewritePending());
            BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
            BOOST_REQUIRE(wallet->Lock());
        }
        CheckNoneInFile(*wallet, secrets);
        Unload(std::move(wallet));
    }
}

// The kind of a wallet whose secrets are known to be in their record does not
// hang on the flag: when the flag cannot be put back at load (case D on a
// medium that cannot be written), it still reads as a mnemonic wallet, so
// that nothing replaces its seed.
BOOST_AUTO_TEST_CASE(kind_kept_when_flag_cannot_be_written)
{
    Unload(Create("flag_fails", walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE));
    Edit("flag_fails", [&](DatabaseBatch& batch) { SetFlag(batch, false); });
    g_wallet_hd_write_fault = [](const std::string& what) { return what == "flags"; };
    std::shared_ptr<CWallet> wallet = Load("flag_fails");
    g_wallet_hd_write_fault = nullptr;
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(!wallet->IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_CHECK(spk_man.IsBip39Enabled() && spk_man.IsBip44Enabled());
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    Addresses(*wallet, 5);
    Unload(std::move(wallet));
}

// A wallet created with a passphrase gets its seed straight into the
// encrypted record, in one transaction. When that transaction fails, the
// creation fails, and nothing of the secrets is in any file it left. (A
// failing flags write stops the creation earlier, at its first flags write;
// the flags write inside the transaction is the one failed_move_changes_nothing
// reaches, through the same function.)
BOOST_AUTO_TEST_CASE(failed_creation_writes_no_secret)
{
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    for (const std::string& failing : {"txnbegin", "flags", "hdsecrets", "hdchain", "txncommit"}) {
        BOOST_TEST_MESSAGE(failing);
        const std::string name{"create_fails_" + failing};
        g_wallet_hd_write_fault = [&](const std::string& what) { return what == failing; };
        {
            DatabaseOptions options;
            options.require_create = true;
            options.require_format = DatabaseFormat::BERKELEY;
            options.create_passphrase = WALLET_PASSPHRASE;
            WalletOptions walletoptions;
            walletoptions.walletType = walletType::bip44Wallet;
            walletoptions.ssMnemonic = PHRASE;
            walletoptions.ssMnemonicPassphrase = PHRASE_PASSPHRASE;
            DatabaseStatus status;
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            std::shared_ptr<CWallet> wallet;
            bool failed{false};
            try {
                wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
                failed = !wallet;
            } catch (const std::runtime_error& e) {
                BOOST_TEST_MESSAGE(e.what());
                failed = true;
            }
            g_wallet_hd_write_fault = nullptr;
            BOOST_CHECK_MESSAGE(failed, "the wallet was created although a write failed");
            if (wallet) Unload(std::move(wallet));
        }
        {
            // The wallet that failed has been let go, its file closed: a file
            // still open in this process could not be opened again.
            SyncWithValidationInterfaceQueue();
            DatabaseOptions options;
            options.require_existing = true;
            DatabaseStatus status;
            bilingual_str error;
            const std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
            BOOST_CHECK_MESSAGE(status != DatabaseStatus::FAILED_ALREADY_LOADED, strprintf("the wallet %s is still open after its creation failed", name));
        }
        for (const auto& bytes : {secrets.phrase, secrets.phrase_passphrase, secrets.seed}) {
            for (fs::recursive_directory_iterator it(GetWalletDir() / name), end; it != end; ++it) {
                if (!fs::is_regular_file(it->path())) continue;
                if (it->path().filename() == ".walletlock") continue; // empty, and locked while its wallet is loaded: Windows refuses to read it
                std::ifstream file(it->path().string(), std::ios::binary);
                const std::vector<unsigned char> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                BOOST_CHECK_MESSAGE(std::search(content.begin(), content.end(), bytes.begin(), bytes.end()) == content.end(),
                                    strprintf("a secret is in %s", it->path().string()));
            }
        }
    }
}

// What the GUI asks around an unlock: whether it rewrites the file (said before
// the call), and what the owner is told after the seed was moved (given once).
BOOST_AUTO_TEST_CASE(gui_is_told_about_the_move)
{
    const CHDChain clear = ClearChain("gui_clear");
    MakeUnmigrated("gui", clear);
    std::shared_ptr<CWallet> wallet = Load("gui", /* expect_warnings */ true);
    BOOST_CHECK(wallet->HDSecretsRewriteOnUnlock());
    BOOST_CHECK(wallet->TakeHDSecretsMovedNotices().empty());
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    BOOST_CHECK(!wallet->HDSecretsRewriteOnUnlock());
    const std::vector<bilingual_str> notices = wallet->TakeHDSecretsMovedNotices();
    // The move, and the wallet's phrase to be written down: it has no keys of an earlier seed.
    BOOST_REQUIRE_EQUAL(notices.size(), 2U);
    BOOST_CHECK(notices[0].original.find("now encrypted with the wallet passphrase") != std::string::npos);
    BOOST_CHECK(notices[1].original.find("This wallet has a mnemonic phrase") != std::string::npos);
    BOOST_CHECK(wallet->TakeHDSecretsMovedNotices().empty());
    BOOST_REQUIRE(wallet->Lock());
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    BOOST_CHECK(wallet->TakeHDSecretsMovedNotices().empty());
    BOOST_REQUIRE(wallet->Lock());
    Unload(std::move(wallet));
}

// A rewrite writes the new file under the wallet file's name with ".rewrite"
// added. A file of that name found there, left by an earlier attempt or put
// there by anyone, is removed first: it is not opened and added to, which put
// another wallet's keys into this one.
BOOST_AUTO_TEST_CASE(rewrite_replaces_a_file_left_in_its_way)
{
    std::shared_ptr<CWallet> other = Create("rw_other", walletType::bip32Wallet);
    const std::set<CKeyID> other_keys = Assert(other->GetLegacyScriptPubKeyMan())->GetKeys();
    BOOST_REQUIRE(!other_keys.empty());
    Unload(std::move(other));
    std::shared_ptr<CWallet> wallet = Create("rw_target", walletType::bip32Wallet);
    Addresses(*wallet, 3);
    const fs::path planted = GetWalletDir() / "rw_target" / "wallet.dat.rewrite";
    fs::copy_file(GetWalletDir() / "rw_other" / "wallet.dat", planted);
    BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
    BOOST_CHECK(!fs::exists(planted));
    Unload(std::move(wallet));

    // The other wallet's keys are not in it. Had they been added, in clear
    // beside this wallet's encrypted keys, it would not load at all.
    wallet = Load("rw_target");
    for (const CKeyID& key : Assert(wallet->GetLegacyScriptPubKeyMan())->GetKeys()) {
        BOOST_CHECK(!other_keys.count(key));
    }
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    Addresses(*wallet, 2);
    BOOST_REQUIRE(wallet->Lock());
    Unload(std::move(wallet));
}

// A record that is already in the new file is a failure of the rewrite, not
// skipped: the old file stays as it was and nothing of the new one is left.
BOOST_AUTO_TEST_CASE(rewrite_fails_on_a_key_already_written)
{
    std::shared_ptr<CWallet> wallet = Create("rw_dup", walletType::bip32Wallet);
    Addresses(*wallet, 3);
    const fs::path file = GetWalletDir() / "rw_dup" / "wallet.dat";
    g_wallet_hd_write_fault = [](const std::string& what) { return what == "rewrite-duplicate"; };
    BOOST_CHECK(!wallet->GetDatabase().Rewrite());
    g_wallet_hd_write_fault = nullptr;
    BOOST_CHECK(fs::exists(file));
    BOOST_CHECK(!fs::exists(GetWalletDir() / "rw_dup" / "wallet.dat.rewrite"));
    Addresses(*wallet, 2);
    BOOST_CHECK(wallet->GetDatabase().Rewrite());
    Unload(std::move(wallet));
    Unload(Load("rw_dup"));
}

// When the new file cannot be made, the old file is untouched, and the rewrite
// that the move of the seed owes stays owed, noted in the file, and is done at
// the next unlock.
BOOST_AUTO_TEST_CASE(rewrite_that_cannot_start_stays_owed)
{
    const CHDChain clear = ClearChain("rw_ro_clear");
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    MakeUnmigrated("rw_ro", clear);
    std::shared_ptr<CWallet> wallet = Load("rw_ro", /* expect_warnings */ true);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    const fs::path directory = GetWalletDir() / "rw_ro";
    // The rewrite is made to fail where it starts. (Taking away the right to
    // write to the directory does not stop root, nor anyone on Windows.)
    g_wallet_hd_write_fault = [](const std::string& what) { return what == "rewrite"; };
    const bool unlocked = wallet->Unlock(WALLET_PASSPHRASE);
    g_wallet_hd_write_fault = nullptr;
    BOOST_REQUIRE(unlocked);
    // Moved, in the old file (which was written in place), and still owed.
    BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
    BOOST_CHECK(spk_man.IsHDSecretsRewritePending());
    BOOST_CHECK(wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
    BOOST_CHECK(!fs::exists(directory / "wallet.dat.rewrite"));
    BOOST_CHECK_EQUAL(wallet->GetHDWarnings().size(), 1U);
    BOOST_REQUIRE(wallet->Lock());
    BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
    BOOST_CHECK(!spk_man.IsHDSecretsRewritePending());
    BOOST_CHECK(!wallet->GetDatabase().MakeBatch()->Exists(DBKeys::HDSECRETSPENDING));
    BOOST_REQUIRE(wallet->Lock());
    CheckNoneInFile(*wallet, secrets);
    Unload(std::move(wallet));
}

// A wallet that an earlier version converted at encryption has keys of its
// former seed beside a chain that holds a seed of its own; its owner is told
// that only the file holds them all. Which text depends on whether it has a
// phrase: W1 with one, W2 without (a chain made from -hdseed), and W3 when
// that cannot be known (moved and locked: the phrase is in the encrypted
// record). It holds from the move on, not from the next load.
BOOST_AUTO_TEST_CASE(earlier_seed_warning_says_what_is_known)
{
    const std::string W1{"Its mnemonic phrase does not restore"};
    const std::string W2{"This wallet has no mnemonic phrase. Its seed does not restore"};
    const std::string W3{"Neither its seed nor its mnemonic phrase, if it has one, restores"};
    const CHDChain clear = ClearChain("es_clear");
    for (const bool with_phrase : {true, false}) {
        const std::string name{strprintf("es_%d", with_phrase)};
        BOOST_TEST_MESSAGE(name);
        // A plain wallet with keys, encrypted; then given the chain an earlier
        // version gave it at encryption, with the secrets in clear in it.
        std::shared_ptr<CWallet> wallet = Create(name, walletType::bip32Wallet);
        Addresses(*wallet, 3);
        BOOST_REQUIRE(wallet->EncryptWallet(WALLET_PASSPHRASE));
        Unload(std::move(wallet));
        CHDChain chain{clear};
        if (!with_phrase) {
            SecureVector().swap(chain.vchMnemonic);
            SecureVector().swap(chain.vchMnemonicPassphrase);
        }
        Edit(name, [&](DatabaseBatch& batch) { BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, chain)); });

        const std::string& known = with_phrase ? W1 : W2;
        const auto says = [](const std::vector<bilingual_str>& notices, const std::string& text) {
            return std::any_of(notices.begin(), notices.end(), [&](const bilingual_str& n) { return n.original.find(text) != std::string::npos; });
        };
        wallet = Load(name, /* expect_warnings */ true);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::UNMIGRATED);
        BOOST_CHECK_EQUAL(spk_man.IsBip39Enabled(), with_phrase);
        BOOST_CHECK(spk_man.HasEarlierSeedKeys());
        BOOST_CHECK(says(wallet->GetHDWarnings(), known));
        // At load: after "Wallet <name>: ", from a small letter, with the advice.
        const std::vector<bilingual_str> at_load = wallet->GetHDWarnings(/* for_load */ true);
        BOOST_CHECK(says(at_load, with_phrase ? "This wallet was converted" : "The HD seed of this wallet was replaced"));
        BOOST_CHECK(says(at_load, "keep a backup of the file, or create a new wallet and move the funds to it."));

        // The unlock moves the seed: from then on it is of the mnemonic kind, and still told.
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        BOOST_CHECK(spk_man.GetHDSecretsState() == HDState::ENCRYPTED);
        BOOST_CHECK(spk_man.IsBip39Enabled());
        BOOST_CHECK(spk_man.HasEarlierSeedKeys());
        BOOST_CHECK(says(wallet->GetHDWarnings(), known));
        BOOST_CHECK(spk_man.GetEarlierSeedWarning().original.find(known) != std::string::npos);
        const std::vector<bilingual_str> moved = wallet->TakeHDSecretsMovedNotices();
        BOOST_CHECK(says(moved, with_phrase ? "you may never have been shown its phrase" : "This wallet has no mnemonic phrase. Its seed restores only"));
        BOOST_CHECK(!says(moved, "This wallet has a mnemonic phrase. If you never wrote one down"));
        if (!with_phrase) {
            for (const auto& notices : {wallet->GetHDWarnings(), wallet->GetHDWarnings(true), moved}) {
                BOOST_CHECK(!says(notices, "mnemonic phrase does not restore") && !says(notices, "its phrase"));
            }
        }
        // Locked, it cannot be known whether there is a phrase.
        BOOST_REQUIRE(wallet->Lock());
        BOOST_CHECK(says(wallet->GetHDWarnings(), W3));
        Unload(std::move(wallet));

        wallet = Load(name, /* expect_warnings */ true);
        BOOST_CHECK(Assert(wallet->GetLegacyScriptPubKeyMan())->HasEarlierSeedKeys());
        BOOST_CHECK(says(wallet->GetHDWarnings(), W3));
        BOOST_CHECK(says(wallet->GetHDWarnings(true), "The HD seed of this wallet was replaced by an earlier version when it was encrypted. Neither"));
        BOOST_REQUIRE(wallet->Unlock(WALLET_PASSPHRASE));
        BOOST_CHECK(says(wallet->GetHDWarnings(), known));
        BOOST_REQUIRE(wallet->Lock());
        Unload(std::move(wallet));
    }

    // A wallet made from a mnemonic, without keys of another seed: none of it.
    std::shared_ptr<CWallet> wallet = Create("es_none", walletType::bip44Wallet, PHRASE_PASSPHRASE, WALLET_PASSPHRASE);
    BOOST_CHECK(!Assert(wallet->GetLegacyScriptPubKeyMan())->HasEarlierSeedKeys());
    BOOST_CHECK(wallet->GetHDWarnings().empty());
    Unload(std::move(wallet));
}

// Every Berkeley DB wallet is rewritten once, at its first load by this
// version, and marked so; a file this version makes is marked from the start.
// A rewrite that fails leaves the wallet as it was, unmarked and said so, and
// the next load tries again. SQLite wallets are left alone.
BOOST_AUTO_TEST_CASE(berkeley_wallet_rewritten_once_at_load)
{
    const auto marked = [&](const std::string& name) {
        bool has{false};
        Edit(name, [&](DatabaseBatch& batch) { has = batch.Exists(DBKeys::REWRITTEN); });
        return has;
    };
    int rewrites{0};
    bool fail{false};
    g_wallet_hd_write_fault = [&](const std::string& what) {
        if (what != "rewrite-file") return false;
        ++rewrites;
        return fail;
    };

    Unload(Create("once", walletType::bip32Wallet));
    BOOST_CHECK(marked("once"));
    BOOST_CHECK_EQUAL(rewrites, 0);
    // As an earlier version left it: no marker.
    Edit("once", [&](DatabaseBatch& batch) { BOOST_REQUIRE(batch.Erase(DBKeys::REWRITTEN)); });

    // Fails: loads all the same, unmarked, and says so.
    fail = true;
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet = LoadWallet(*m_node.chain, "once", std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK_EQUAL(rewrites, 1);
        BOOST_REQUIRE_EQUAL(warnings.size(), 1U);
        BOOST_CHECK(warnings[0].original.find("could not be rewritten") != std::string::npos);
        Addresses(*wallet, 2);
        Unload(std::move(wallet));
    }
    BOOST_CHECK(!marked("once"));

    // The next load does it, and says so once.
    fail = false;
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet = LoadWallet(*m_node.chain, "once", std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK_EQUAL(rewrites, 2);
        BOOST_REQUIRE_EQUAL(warnings.size(), 1U);
        BOOST_CHECK(warnings[0].original.find("has been rewritten") != std::string::npos);
        Unload(std::move(wallet));
    }
    BOOST_CHECK(marked("once"));
    // And not again.
    Unload(Load("once"));
    BOOST_CHECK_EQUAL(rewrites, 2);

    // A SQLite wallet: never rewritten, never marked.
    Unload(Create("once_sqlite", walletType::bip32Wallet, "", "", DatabaseFormat::SQLITE));
    Unload(Load("once_sqlite"));
    BOOST_CHECK_EQUAL(rewrites, 2);
    BOOST_CHECK(!marked("once_sqlite"));
    g_wallet_hd_write_fault = nullptr;
}

// A wallet that had a mnemonic chain and was then given a plain seed with
// sethdseed by an earlier version is a plain wallet now, and no move of its
// seed reaches it; the old phrase may still be in the file, where the old
// chain record was. The rewrite at its first load removes it.
BOOST_AUTO_TEST_CASE(old_phrase_of_a_reseeded_wallet_gone_after_first_load)
{
    const Secrets secrets = SecretsOf(PHRASE_PASSPHRASE);
    Unload(Create("reseeded", walletType::bip44Wallet, PHRASE_PASSPHRASE));
    // What sethdseed left: the chain record of a plain seed in place of the
    // mnemonic one (the seed id stands for the new seed), and no marker.
    Edit("reseeded", [&](DatabaseBatch& batch) {
        CHDChain plain;
        plain.nVersion = CHDChain::VERSION_HD_CHAIN_SPLIT;
        plain.seed_id = CKeyID(uint160(ParseHex("5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a")));
        BOOST_REQUIRE(batch.Write(DBKeys::HDCHAIN, plain));
        BOOST_REQUIRE(batch.Erase(DBKeys::REWRITTEN));
    });
    bool before{false};
    for (fs::recursive_directory_iterator it(GetWalletDir() / "reseeded"), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path())) continue;
        if (it->path().filename() == ".walletlock") continue; // empty, and locked while its wallet is loaded: Windows refuses to read it
        std::ifstream file(it->path().string(), std::ios::binary);
        const std::vector<unsigned char> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        before |= std::search(content.begin(), content.end(), secrets.phrase.begin(), secrets.phrase.end()) != content.end();
    }
    BOOST_REQUIRE_MESSAGE(before, "the old phrase is not in the file to begin with");
    std::shared_ptr<CWallet> wallet = Load("reseeded", /* expect_warnings */ true);
    BOOST_CHECK(FilesHolding(*wallet, secrets.phrase).empty());
    BOOST_CHECK(FilesHolding(*wallet, secrets.seed).empty());
    Unload(std::move(wallet));
}

// A change of passphrase writes the master key, encrypted with the new
// passphrase, over the old record, which the old passphrase still opens. The
// file is rewritten so that the old one is in no file of the wallet. A rewrite
// that fails leaves a Berkeley DB file unmarked, and the next load does it.
BOOST_AUTO_TEST_CASE(old_master_key_gone_after_passphrase_change)
{
    const SecureString new_passphrase{"the passphrase the wallet is encrypted with now"};
    const auto crypted_key = [](CWallet& wallet) {
        LOCK(wallet.cs_wallet);
        BOOST_REQUIRE_EQUAL(wallet.mapMasterKeys.size(), 1U);
        return wallet.mapMasterKeys.begin()->second.vchCryptedKey;
    };
    const auto marked = [&](const std::string& name) {
        bool has{false};
        Edit(name, [&](DatabaseBatch& batch) { has = batch.Exists(DBKeys::REWRITTEN); });
        return has;
    };
    int rewrites{0};
    bool fail{false};
    g_wallet_hd_write_fault = [&](const std::string& what) {
        if (what != "rewrite-file") return false;
        ++rewrites;
        return fail;
    };
    for (const auto& [format, rewrite_fails] : {std::make_pair(DatabaseFormat::BERKELEY, false), std::make_pair(DatabaseFormat::BERKELEY, true), std::make_pair(DatabaseFormat::SQLITE, false)}) {
        const bool bdb{format == DatabaseFormat::BERKELEY};
        const std::string name{strprintf("passphrase_change_%s_%d", bdb ? "bdb" : "sqlite", rewrite_fails)};
        std::shared_ptr<CWallet> wallet = Create(name, walletType::bip32Wallet, "", WALLET_PASSPHRASE, format);
        Addresses(*wallet, 2);
        const std::vector<unsigned char> old_key = crypted_key(*wallet);
        BOOST_REQUIRE(!FilesHolding(*wallet, old_key).empty());

        rewrites = 0;
        fail = rewrite_fails;
        BOOST_REQUIRE(wallet->ChangeWalletPassphrase(WALLET_PASSPHRASE, new_passphrase));
        fail = false;
        BOOST_CHECK_EQUAL(rewrites, 1);
        const std::vector<unsigned char> new_key = crypted_key(*wallet);
        BOOST_CHECK(new_key != old_key);
        BOOST_CHECK(wallet->IsLocked());
        BOOST_CHECK(!wallet->Unlock(WALLET_PASSPHRASE));
        BOOST_REQUIRE(wallet->Unlock(new_passphrase));
        BOOST_REQUIRE(wallet->Lock());
        BOOST_CHECK(!FilesHolding(*wallet, new_key).empty());
        // Without the rewrite the old key is still there: what the test looks for can be found.
        BOOST_CHECK_EQUAL(FilesHolding(*wallet, old_key).empty(), !rewrite_fails);
        Unload(std::move(wallet));
        // Only a Berkeley DB file is marked; one whose rewrite failed is not, until its next load.
        BOOST_CHECK_EQUAL(marked(name), bdb && !rewrite_fails);

        wallet = Load(name, /* expect_warnings */ rewrite_fails);
        BOOST_CHECK_EQUAL(rewrites, rewrite_fails ? 2 : 1);
        BOOST_CHECK(FilesHolding(*wallet, old_key).empty());
        BOOST_REQUIRE(wallet->Unlock(new_passphrase));
        Unload(std::move(wallet));
        BOOST_CHECK_EQUAL(marked(name), bdb);
    }
    g_wallet_hd_write_fault = nullptr;
}

BOOST_AUTO_TEST_SUITE_END()
