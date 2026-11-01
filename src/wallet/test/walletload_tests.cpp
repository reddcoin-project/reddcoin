// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <outputtype.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/check.h>
#include <util/system.h>
#include <util/translation.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/bip39.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {
//! A chain, and wallets in files that are created, closed and loaded again.
struct WalletLoadTestingSetup : public TestChain100Setup {
    WalletLoadTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
    }

    std::shared_ptr<CWallet> Create(const std::string& name, const SecureString& passphrase = "", int wallet_type = walletType::bip32Wallet)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
        walletoptions.walletType = wallet_type;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        options.create_passphrase = passphrase;
        std::shared_ptr<CWallet> wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }

    std::shared_ptr<CWallet> Load(const std::string& name, bilingual_str& error, std::vector<bilingual_str>& warnings)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        options.require_existing = true;
        return LoadWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, status, error, warnings);
    }

    void Unload(std::shared_ptr<CWallet>&& wallet)
    {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(RemoveWallet(wallet, /* load_on_start */ std::nullopt));
        UnloadWallet(std::move(wallet));
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(walletload_tests, WalletLoadTestingSetup)

// A wallet with a plain HD seed whose keys are encrypted is locked when it is
// loaded, and the seed cannot be read then. It is still an HD wallet with the
// seed it had, it loads without a warning, and once unlocked it derives keys.
BOOST_AUTO_TEST_CASE(encrypted_wallet_keeps_its_hd_seed_when_loaded)
{
    std::shared_ptr<CWallet> wallet = Create("encrypted", "passphrase");
    const CKeyID seed_id = Assert(wallet->GetLegacyScriptPubKeyMan())->GetHDChain().seed_id;
    BOOST_CHECK(!seed_id.IsNull());
    Unload(std::move(wallet));

    bilingual_str error;
    std::vector<bilingual_str> warnings;
    wallet = Load("encrypted", error, warnings);
    BOOST_REQUIRE_MESSAGE(wallet, error.original);
    const std::string warning{warnings.empty() ? "" : warnings[0].original};
    BOOST_CHECK_MESSAGE(warnings.empty(), warning);
    BOOST_CHECK(wallet->IsLocked());
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(spk_man.IsHDEnabled());
    BOOST_CHECK(spk_man.GetHDChain().seed_id == seed_id);

    // More addresses than the key pool holds: the rest are derived.
    BOOST_REQUIRE(wallet->Unlock("passphrase"));
    BOOST_CHECK(wallet->TopUpKeyPool());
    for (int i = 0; i < 6; ++i) {
        CTxDestination dest;
        std::string failure;
        BOOST_REQUIRE_MESSAGE(wallet->GetNewDestination(OutputType::LEGACY, "", dest, failure), failure);
        LOCK(spk_man.cs_KeyStore);
        BOOST_CHECK(spk_man.mapKeyMetadata.at(ToKeyID(std::get<PKHash>(dest))).hd_seed_id == seed_id);
    }
    Unload(std::move(wallet));
}

// A wallet whose load fails after it has been attached to the chain is
// released: it is not left behind holding its file, so that the next attempt
// is told "already loaded" for a wallet that nothing lists.
BOOST_AUTO_TEST_CASE(wallet_that_fails_to_load_is_released)
{
    Unload(Create("rescan_fails"));

    // The wallet is made to read the chain again, and cannot: the file of the
    // blocks is gone.
    gArgs.ForceSetArg("-rescan", "1");
    const int file{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockPos().nFile)};
    WITH_LOCK(cs_main, Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file));
    UnlinkPrunedFiles({file});

    for (int attempt = 0; attempt < 2; ++attempt) {
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        BOOST_CHECK(!Load("rescan_fails", error, warnings));
        BOOST_CHECK_MESSAGE(error.original.find("Failed to rescan the wallet during initialization") != std::string::npos, error.original);
        BOOST_CHECK(!GetWallet("rescan_fails"));
    }
}

// -hdseed gave the next mnemonic wallet created the seed it named, unchecked
// and without a phrase, and -mnemonic and -mnemonicpassphrase were read and
// not used. None of them is read: a new mnemonic wallet gets a phrase of its
// own, and its seed is the one that phrase gives.
BOOST_AUTO_TEST_CASE(seed_options_do_not_reach_a_new_wallet)
{
    const std::string seed_hex(128, 'a');
    const SecureString other_phrase{CMnemonic::Generate(256)};
    gArgs.ForceSetArg("-hdseed", seed_hex);
    gArgs.ForceSetArg("-mnemonic", std::string(other_phrase.begin(), other_phrase.end()));
    gArgs.ForceSetArg("-mnemonicpassphrase", "a passphrase");
    for (const int type : {walletType::bip39Wallet, walletType::bip44Wallet}) {
        const std::string name{strprintf("seed_options_%d", type)};
        BOOST_TEST_MESSAGE(name);
        std::shared_ptr<CWallet> wallet = Create(name, "", type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        CHDChain chain;
        {
            LOCK(spk_man.cs_KeyStore);
            chain = spk_man.GetHDChain();
        }
        BOOST_CHECK(spk_man.IsBip39Enabled());
        BOOST_CHECK(!chain.vchMnemonic.empty());
        BOOST_CHECK(chain.vchMnemonicPassphrase.empty());
        const SecureString phrase(chain.vchMnemonic.begin(), chain.vchMnemonic.end());
        BOOST_CHECK(phrase != other_phrase);
        SecureVector seed;
        CMnemonic::ToSeed(phrase, "", seed);
        BOOST_CHECK(chain.vchSeed == seed);
        BOOST_CHECK(HexStr(chain.vchSeed) != seed_hex);
        Unload(std::move(wallet));
    }
    gArgs.LockSettings([](util::Settings& settings) {
        for (const char* option : {"hdseed", "mnemonic", "mnemonicpassphrase"}) settings.forced_settings.erase(option);
    });
}

BOOST_AUTO_TEST_SUITE_END()
