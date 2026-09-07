// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <key.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(scriptpubkeyman_tests, BasicTestingSetup)

// Test LegacyScriptPubKeyMan::CanProvide behavior, making sure it returns true
// for recognized scripts even when keys may not be available for signing.
BOOST_AUTO_TEST_CASE(CanProvide)
{
    // Set up wallet and keyman variables.
    CWallet wallet(m_node.chain.get(), "", CreateDummyWalletDatabase());
    LegacyScriptPubKeyMan& keyman = *wallet.GetOrCreateLegacyScriptPubKeyMan();

    // Make a 1 of 2 multisig script
    std::vector<CKey> keys(2);
    std::vector<CPubKey> pubkeys;
    for (CKey& key : keys) {
        key.MakeNewKey(true);
        pubkeys.emplace_back(key.GetPubKey());
    }
    CScript multisig_script = GetScriptForMultisig(1, pubkeys);
    CScript p2sh_script = GetScriptForDestination(ScriptHash(multisig_script));
    SignatureData data;

    // Verify the p2sh(multisig) script is not recognized until the multisig
    // script is added to the keystore to make it solvable
    BOOST_CHECK(!keyman.CanProvide(p2sh_script, data));
    keyman.AddCScript(multisig_script);
    BOOST_CHECK(keyman.CanProvide(p2sh_script, data));
}

// Reddcoin: descriptor wallets must derive under Reddcoin's registered
// SLIP-0044 coin type, the same one legacy HD wallets use, on every chain.
// They used to derive under Bitcoin's hardcoded 0' on mainnet, which put the
// two wallet types in different places for one seed.
BOOST_AUTO_TEST_CASE(DescriptorCoinType)
{
    constexpr uint32_t HARDENED = 0x80000000;
    const std::vector<std::pair<OutputType, uint32_t>> purposes{
        {OutputType::LEGACY, 44},
        {OutputType::P2SH_SEGWIT, 49},
        {OutputType::BECH32, 84},
    };

    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        SelectParams(chain);
        const uint32_t coin_type = Params().ExtCoinType();

        for (const auto& [addr_type, purpose] : purposes) {
            CWallet wallet(m_node.chain.get(), "", CreateMockWalletDatabase());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            LOCK(wallet.cs_wallet);

            CKey master_key;
            master_key.MakeNewKey(true);
            CExtKey master_ext;
            master_ext.SetSeed(master_key.begin(), master_key.size());

            auto desc_spk_man = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(wallet));
            BOOST_CHECK(desc_spk_man->SetupDescriptorGeneration(master_ext, addr_type, false));

            // Check the path the wallet reports for an address it just handed
            // out, which is what getaddressinfo shows the user.
            CTxDestination dest;
            std::string error;
            BOOST_CHECK(desc_spk_man->GetNewDestination(addr_type, dest, error));

            std::unique_ptr<CKeyMetadata> meta = desc_spk_man->GetMetadata(dest);
            BOOST_REQUIRE(meta);
            BOOST_REQUIRE(meta->has_key_origin);
            BOOST_REQUIRE_EQUAL(meta->key_origin.path.size(), 5U);
            BOOST_CHECK_EQUAL(meta->key_origin.path[0], purpose | HARDENED);
            BOOST_CHECK_EQUAL(meta->key_origin.path[1], coin_type | HARDENED);
            BOOST_CHECK_EQUAL(meta->key_origin.path[2], 0 | HARDENED);
        }
    }

    // Restore the chain the unit tests run on.
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_SUITE_END()
