// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

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

// Test DescriptorScriptPubKeyMan::GetKey behavior for retrieving private keys
BOOST_AUTO_TEST_CASE(DescriptorGetKey)
{
    // Create a wallet with descriptor flag set
    CWallet wallet(m_node.chain.get(), "", CreateMockWalletDatabase());
    wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    LOCK(wallet.cs_wallet);

    // Create a master key
    CKey master_key;
    master_key.MakeNewKey(true);
    CExtKey master_ext;
    master_ext.SetSeed(master_key.begin(), master_key.size());

    // Create and setup a DescriptorScriptPubKeyMan with a pkh descriptor
    auto desc_spk_man = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(wallet));
    BOOST_CHECK(desc_spk_man->SetupDescriptorGeneration(master_ext, OutputType::LEGACY, false));

    // Get a destination from the descriptor
    CTxDestination dest;
    std::string error;
    BOOST_CHECK(desc_spk_man->GetNewDestination(OutputType::LEGACY, dest, error));

    // Convert destination to script
    CScript script = GetScriptForDestination(dest);

    // Extract the keyid from the destination (it's a PKHash for LEGACY type)
    BOOST_CHECK(std::holds_alternative<PKHash>(dest));
    CKeyID keyid = ToKeyID(std::get<PKHash>(dest));

    // Test GetKey succeeds for a key we own
    CKey retrieved_key;
    BOOST_CHECK(desc_spk_man->GetKey(script, keyid, retrieved_key));
    BOOST_CHECK(retrieved_key.IsValid());

    // Verify the retrieved key corresponds to the keyid
    BOOST_CHECK(retrieved_key.GetPubKey().GetID() == keyid);

    // Test GetKey fails for a key we don't own
    CKey random_key;
    random_key.MakeNewKey(true);
    CKeyID random_keyid = random_key.GetPubKey().GetID();
    CScript random_script = GetScriptForDestination(PKHash(random_keyid));
    CKey not_found_key;
    BOOST_CHECK(!desc_spk_man->GetKey(random_script, random_keyid, not_found_key));
}

BOOST_AUTO_TEST_SUITE_END()
