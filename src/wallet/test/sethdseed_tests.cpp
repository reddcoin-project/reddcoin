// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/server.h>
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
#include <univalue.h>

#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace {
//! A chain, the wallet commands, and wallets made the way the wallet creation
//! dialog makes them: this version has no command that creates a wallet from
//! a mnemonic.
struct HDSeedTestingSetup : public TestChain100Setup {
    HDSeedTestingSetup()
    {
        gArgs.ForceSetArg("-keypool", "3");
        m_wallet_client->registerRpcs();
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
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

    //! Run a wallet command the way the RPC server does.
    UniValue Call(const CWallet& wallet, const std::string& method, const UniValue& params = UniValue(UniValue::VARR))
    {
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = params;
        request.URI = "/wallet/" + wallet.GetName();
        try {
            return tableRPC.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(strprintf("%d %s", find_value(error, "code").get_int(), find_value(error, "message").get_str()));
        }
    }

    //! What a command fails with, code and message, or nothing.
    std::string Failure(const CWallet& wallet, const std::string& method, const UniValue& params)
    {
        try {
            Call(wallet, method, params);
        } catch (const std::runtime_error& error) {
            return error.what();
        }
        return "";
    }

    //! The path of the key of a new address.
    std::string NextPath(CWallet& wallet)
    {
        CTxDestination dest;
        std::string error;
        BOOST_REQUIRE(wallet.GetNewDestination(OutputType::LEGACY, "", dest, error));
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet.GetLegacyScriptPubKeyMan());
        LOCK(spk_man.cs_KeyStore);
        return spk_man.mapKeyMetadata.at(ToKeyID(std::get<PKHash>(dest))).hdKeypath;
    }

    std::unique_ptr<interfaces::WalletClient> m_wallet_client = interfaces::MakeWalletClient(*m_node.chain, *Assert(m_node.args));
};

UniValue Params(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const UniValue& value : values) params.push_back(value);
    return params;
}

std::string NewSecret()
{
    CKey key;
    key.MakeNewKey(/* compressed */ true);
    return EncodeSecret(key);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sethdseed_tests, HDSeedTestingSetup)

// sethdseed on a wallet created from a mnemonic is refused, whatever its
// arguments, and leaves the wallet as it was.
BOOST_AUTO_TEST_CASE(refused_on_a_wallet_created_from_a_mnemonic)
{
    for (const int wallet_type : {walletType::bip39Wallet, walletType::bip44Wallet}) {
        const bool bip44{wallet_type == walletType::bip44Wallet};
        std::shared_ptr<CWallet> wallet = Create(bip44 ? "bip44" : "bip39", wallet_type);
        LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
        BOOST_CHECK(spk_man.IsBip39Enabled());
        BOOST_CHECK_EQUAL(spk_man.IsBip44Enabled(), bip44);
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/0" : "m/0'/0'/0'");
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/1" : "m/0'/0'/1'");

        const CHDChain before = spk_man.GetHDChain();
        const std::set<CKeyID> keys_before = spk_man.GetKeys();
        const unsigned int keypool_before = WITH_LOCK(wallet->cs_wallet, return wallet->GetKeyPoolSize());
        const std::string info_before = Call(*wallet, "gethdwalletinfo").write();
        BOOST_CHECK(!before.vchMnemonic.empty());
        BOOST_CHECK(info_before.find("\"mnemonic\"") != std::string::npos);

        for (const UniValue& params : {Params({}), Params({true}), Params({false}), Params({true, NewSecret()}), Params({false, NewSecret()})}) {
            const std::string failure = Failure(*wallet, "sethdseed", params);
            BOOST_CHECK_MESSAGE(failure.find(strprintf("%d Cannot set a new HD seed on a wallet created from a mnemonic", RPC_WALLET_ERROR)) == 0, failure);
        }

        const CHDChain& after = spk_man.GetHDChain();
        BOOST_CHECK(after.seed_id == before.seed_id);
        BOOST_CHECK(after.vchMnemonic == before.vchMnemonic);
        BOOST_CHECK(after.vchMnemonicPassphrase == before.vchMnemonicPassphrase);
        BOOST_CHECK_EQUAL(after.IsBip44(), bip44);
        BOOST_CHECK_EQUAL(after.nVersion, before.nVersion);
        BOOST_CHECK_EQUAL(after.nExternalChainCounter, before.nExternalChainCounter);
        BOOST_CHECK_EQUAL(after.nInternalChainCounter, before.nInternalChainCounter);
        BOOST_CHECK(spk_man.GetKeys() == keys_before);
        BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->GetKeyPoolSize()), keypool_before);
        BOOST_CHECK_EQUAL(Call(*wallet, "gethdwalletinfo").write(), info_before);
        // The next address is the next one of the phrase.
        BOOST_CHECK_EQUAL(NextPath(*wallet), bip44 ? "m/44'/1'/0'/0/2" : "m/0'/0'/2'");
        Unload(std::move(wallet));
    }
}

// It goes on working on a wallet with a plain HD seed, and on a blank one.
BOOST_AUTO_TEST_CASE(accepted_on_a_plain_seed_and_on_a_blank_wallet)
{
    std::shared_ptr<CWallet> wallet = Create("bip32", walletType::bip32Wallet);
    LegacyScriptPubKeyMan& spk_man = *Assert(wallet->GetLegacyScriptPubKeyMan());
    BOOST_CHECK(spk_man.IsHDEnabled() && !spk_man.IsBip39Enabled());
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/1'");
    const CKeyID first = spk_man.GetHDChain().seed_id;
    const std::set<CKeyID> keys = spk_man.GetKeys();

    BOOST_CHECK(Call(*wallet, "sethdseed").isNull());
    const CKeyID second = spk_man.GetHDChain().seed_id;
    BOOST_CHECK(second != first);
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    // The keys of the first seed are still the wallet's.
    for (const CKeyID& key : keys) BOOST_CHECK(spk_man.HaveKey(key));

    CKey seed;
    seed.MakeNewKey(/* compressed */ true);
    BOOST_CHECK(Call(*wallet, "sethdseed", Params({true, EncodeSecret(seed)})).isNull());
    BOOST_CHECK(spk_man.GetHDChain().seed_id == seed.GetPubKey().GetID());
    BOOST_CHECK(!spk_man.IsBip39Enabled());
    Unload(std::move(wallet));

    wallet = Create("blank", walletType::bip32Wallet, WALLET_FLAG_BLANK_WALLET);
    LegacyScriptPubKeyMan& blank = *Assert(wallet->GetOrCreateLegacyScriptPubKeyMan());
    BOOST_CHECK(!blank.IsHDEnabled());
    BOOST_CHECK(Call(*wallet, "sethdseed").isNull());
    BOOST_CHECK(blank.IsHDEnabled() && !blank.IsBip39Enabled());
    BOOST_CHECK_EQUAL(NextPath(*wallet), "m/0'/0'/0'");
    Unload(std::move(wallet));
}

BOOST_AUTO_TEST_SUITE_END()
