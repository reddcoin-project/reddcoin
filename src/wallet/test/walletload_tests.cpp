// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
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

    std::shared_ptr<CWallet> Create(const std::string& name)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        WalletOptions walletoptions;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        options.require_create = true;
        std::shared_ptr<CWallet> wallet = CreateWallet(*m_node.chain, name, /* load_on_start */ std::nullopt, options, walletoptions, status, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }

    std::shared_ptr<CWallet> Load(const std::string& name, bilingual_str& error)
    {
        DatabaseOptions options;
        DatabaseStatus status;
        std::vector<bilingual_str> warnings;
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
        BOOST_CHECK(!Load("rescan_fails", error));
        BOOST_CHECK_MESSAGE(error.original.find("Failed to rescan the wallet during initialization") != std::string::npos, error.original);
        BOOST_CHECK(!GetWallet("rescan_fails"));
    }
}

BOOST_AUTO_TEST_SUITE_END()
