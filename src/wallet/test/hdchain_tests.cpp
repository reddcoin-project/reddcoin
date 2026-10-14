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
#include <fstream>
#include <iterator>
#include <thread>
#include <memory>
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

    std::shared_ptr<CWallet> Load(const std::string& name)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet = LoadWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        BOOST_CHECK(warnings.empty());
        return wallet;
    }

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
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

BOOST_AUTO_TEST_SUITE_END()
