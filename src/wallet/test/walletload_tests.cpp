// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <node/context.h>
#include <outputtype.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/system.h>
#include <util/translation.h>
#include <validationinterface.h>
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

    std::shared_ptr<CWallet> Create(const std::string& name, const SecureString& passphrase = "")
    {
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
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

BOOST_AUTO_TEST_SUITE_END()
