// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

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

    //! The keys of a seed that the wallet holds: how many, and the last of each chain.
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
            if (meta.hd_seed_id != seed_id || (path.size() != 3 && path.size() != 5)) continue;
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
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdseed_tests, HDSeedTestingSetup)

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

BOOST_AUTO_TEST_SUITE_END()
