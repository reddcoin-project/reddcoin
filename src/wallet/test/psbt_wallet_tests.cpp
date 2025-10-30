// Copyright (c) 2017-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>
#include <script/script.h>
#include <script/standard.h>
#include <util/bip32.h>
#include <util/strencodings.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>
#include <wallet/test/wallet_test_fixture.h>

BOOST_FIXTURE_TEST_SUITE(psbt_wallet_tests, WalletTestingSetup)

// Reddcoin PSBT Test Vectors
// These are hardcoded test vectors for Reddcoin-specific PSBT formats

// V1 PoW transaction hex (no nTime field)
static const std::string REDDCOIN_V1_POW_TX = "0100000001aad73931018bd25f84ae400b68848be09db706eac2ac18298babee71ab656f8b0000000048473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01feffffff0280f0fa020000000017a9140fb9463421696b82c833af241c78c17ddbde493487d0f20a270100000017a91429ca74f8a08f81999428185c97b5d852e4063f618765000000";

// V2 PoS transaction hex (with nTime field)
static const std::string REDDCOIN_V2_POS_TX = "0200000001d77a3931018bd25f84ae400b68848be09db706eac2ac18298babee71ab656f8b0000000048473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01feffffff01e0e4f9020000000017a9140fb9463421696b82c833af241c78c17ddbde4934879600000000345f5f";

// V2 SegWit transaction hex (with witness data and nTime)
static const std::string REDDCOIN_V2_SEGWIT_TX = "0200000000010158e87a21b56daf0c23be8e7070456c336f7cbaa5c8757924f545887bb2abdd7501000000171600145f275f436b09a8cc9a2eb2a2f528485c68a56323feffffff02d8231f1b0100000017a914aed962d6654f9a2b36608eb9d64d2b260db4f1118700c2eb0b0000000017a914b7f5faf40e3d40a5a459b1db3535f2b72fa921e88702483045022100a22edcc6e5bc511af4cc4ae0de0fcd75c7e04d8c1c3a8aa9d820ed4b967384ec02200642963597b9b1bc22c75e9f3e117284a962188bf5e8a74c895089046a20ad770121035509a48eb623e10aace8bfd0212fdb8a8e5af3c94b0b133b95e114cab89e4f796500000000345f5f";

// 2-of-2 multisig redeem script
static const std::string REDDCOIN_REDEEM_SCRIPT = "475221029583bf39ae0a609747ad199addd634fa6108559d6c5cd39b4c2183f1ab96e07f2102dab61ff49a14db6a7d02b0cd1fbb78fc4b18312b5b4e54dae4dba2fbfef536d752ae";

// 2-of-2 multisig witness script
static const std::string REDDCOIN_WITNESS_SCRIPT = "47522103089dc10c7ac6db54f91329af617333db388cead0c231f723379d1b99030b02dc21023add904f3d6dcf59ddb906b0dee23529b7ffb9ed50e5e86151926860221f0e7352ae";

// Reddcoin WIF private key (converted from Bitcoin test key)
static const std::string REDDCOIN_TEST_WIF = "7N1r4YVRMcW6AY8qVv5PUKf6MLG9uZfJvVfcsY6T93F2KCcv69u";

// Test PSBT with v1 PoW transaction (no nTime field, no witness)
BOOST_AUTO_TEST_CASE(psbt_pow_v1_test)
{
    auto spk_man = m_wallet.GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(m_wallet.cs_wallet, spk_man->cs_KeyStore);

    // Create a v1 prevtx (PoW format, no nTime)
    CDataStream s_prev_tx(ParseHex("0100000001aad73931018bd25f84ae400b68848be09db706eac2ac18298babee71ab656f8b0000000048473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01feffffff0280f0fa020000000017a9140fb9463421696b82c833af241c78c17ddbde493487d0f20a270100000017a91429ca74f8a08f81999428185c97b5d852e4063f618765000000"), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef prev_tx;
    s_prev_tx >> prev_tx;
    BOOST_CHECK_EQUAL(prev_tx->nVersion, 1);
    BOOST_CHECK_EQUAL(prev_tx->nTime, 0);  // v1 has no timestamp
    m_wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(prev_tx->GetHash()), std::forward_as_tuple(&m_wallet, prev_tx));

    // Add redeem script for P2SH
    CScript rs;
    CDataStream s_rs(ParseHex("475221029583bf39ae0a609747ad199addd634fa6108559d6c5cd39b4c2183f1ab96e07f2102dab61ff49a14db6a7d02b0cd1fbb78fc4b18312b5b4e54dae4dba2fbfef536d752ae"), SER_NETWORK, PROTOCOL_VERSION);
    s_rs >> rs;
    spk_man->AddCScript(rs);

    // Add HD seed (Reddcoin WIF format)
    CKey key = DecodeSecret("7N1r4YVRMcW6AY8qVv5PUKf6MLG9uZfJvVfcsY6T93F2KCcv69u");
    BOOST_REQUIRE(key.IsValid());
    CPubKey master_pub_key = spk_man->DeriveNewSeed(key);
    spk_man->SetHDSeed(master_pub_key);
    spk_man->NewKeyPool();

    // Create PSBT with v1 transaction
    CMutableTransaction mtx;
    mtx.nVersion = 1;
    mtx.nTime = 0;  // v1 has no timestamp
    mtx.vin.resize(1);
    mtx.vin[0].prevout.hash = prev_tx->GetHash();
    mtx.vin[0].prevout.n = 0;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 50000000;
    mtx.vout[0].scriptPubKey = GetScriptForDestination(ScriptHash(rs));

    PartiallySignedTransaction psbtx(mtx);

    // Verify FillPSBT works with v1 PoW transaction
    bool complete = false;
    TransactionError err = m_wallet.FillPSBT(psbtx, complete, SIGHASH_ALL, false, true);
    BOOST_CHECK_EQUAL(err, TransactionError::OK);

    // Verify transaction version is preserved
    BOOST_CHECK(psbtx.tx.has_value());
    BOOST_CHECK_EQUAL(psbtx.tx->nVersion, 1);
    BOOST_CHECK_EQUAL(psbtx.tx->nTime, 0);

    // Test invalid input detection
    psbtx.tx.value().vin[0].prevout.n = 999;
    PartiallySignedTransaction psbtx_invalid(psbtx.tx.value());
    err = m_wallet.FillPSBT(psbtx_invalid, complete, SIGHASH_ALL, false, true);
    BOOST_CHECK(err != TransactionError::OK);
}

// Test PSBT with v2+ PoS transaction (with nTime field, no witness)
BOOST_AUTO_TEST_CASE(psbt_pos_v2_test)
{
    auto spk_man = m_wallet.GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(m_wallet.cs_wallet, spk_man->cs_KeyStore);

    // Create a v2 prevtx (PoS format, with nTime)
    // This is a manually constructed v2 transaction with nTime field
    CMutableTransaction prev_mtx;
    prev_mtx.nVersion = 2;
    prev_mtx.nTime = 1600000000;  // PoS timestamp
    prev_mtx.vin.resize(1);
    prev_mtx.vin[0].prevout.hash = uint256S("8b6f65ab71eeba8b2918acc2ea06b79de08b84680b40ae845fd28b0131397ad7");
    {
        auto sig = ParseHex("473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01");
        prev_mtx.vin[0].scriptSig = CScript(sig.begin(), sig.end());
    }
    prev_mtx.vout.resize(2);
    prev_mtx.vout[0].nValue = 50000000;
    {
        auto spk = ParseHex("a9140fb9463421696b82c833af241c78c17ddbde493487");
        prev_mtx.vout[0].scriptPubKey = CScript(spk.begin(), spk.end());
    }
    prev_mtx.vout[1].nValue = 4999950000;
    {
        auto spk = ParseHex("a91429ca74f8a08f81999428185c97b5d852e4063f6187");
        prev_mtx.vout[1].scriptPubKey = CScript(spk.begin(), spk.end());
    }
    prev_mtx.nLockTime = 101;

    CTransactionRef prev_tx = MakeTransactionRef(prev_mtx);
    BOOST_CHECK_EQUAL(prev_tx->nVersion, 2);
    BOOST_CHECK_EQUAL(prev_tx->nTime, 1600000000);
    m_wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(prev_tx->GetHash()), std::forward_as_tuple(&m_wallet, prev_tx));

    // Add redeem script
    CScript rs;
    CDataStream s_rs(ParseHex("475221029583bf39ae0a609747ad199addd634fa6108559d6c5cd39b4c2183f1ab96e07f2102dab61ff49a14db6a7d02b0cd1fbb78fc4b18312b5b4e54dae4dba2fbfef536d752ae"), SER_NETWORK, PROTOCOL_VERSION);
    s_rs >> rs;
    spk_man->AddCScript(rs);

    // Add HD seed
    CKey key = DecodeSecret("7N1r4YVRMcW6AY8qVv5PUKf6MLG9uZfJvVfcsY6T93F2KCcv69u");
    BOOST_REQUIRE(key.IsValid());
    CPubKey master_pub_key = spk_man->DeriveNewSeed(key);
    spk_man->SetHDSeed(master_pub_key);
    spk_man->NewKeyPool();

    // Create PSBT with v2 PoS transaction
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nTime = 1600000100;  // PoS timestamp
    mtx.vin.resize(1);
    mtx.vin[0].prevout.hash = prev_tx->GetHash();
    mtx.vin[0].prevout.n = 0;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 49900000;
    mtx.vout[0].scriptPubKey = GetScriptForDestination(ScriptHash(rs));

    PartiallySignedTransaction psbtx(mtx);

    // Verify FillPSBT works with v2 PoS transaction
    bool complete = false;
    TransactionError err = m_wallet.FillPSBT(psbtx, complete, SIGHASH_ALL, false, true);
    BOOST_CHECK_EQUAL(err, TransactionError::OK);

    // Verify transaction version and timestamp are preserved
    BOOST_CHECK(psbtx.tx.has_value());
    BOOST_CHECK_EQUAL(psbtx.tx->nVersion, 2);
    BOOST_CHECK_EQUAL(psbtx.tx->nTime, 1600000100);

    // Test that PSBT can be serialized and deserialized with v2 transaction
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    PartiallySignedTransaction psbtx_copy;
    ss >> psbtx_copy;
    BOOST_CHECK(psbtx_copy.tx.has_value());
    BOOST_CHECK_EQUAL(psbtx_copy.tx->nVersion, 2);
    BOOST_CHECK_EQUAL(psbtx_copy.tx->nTime, 1600000100);
}

// Test PSBT with SegWit transaction (witness data)
BOOST_AUTO_TEST_CASE(psbt_segwit_test)
{
    auto spk_man = m_wallet.GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(m_wallet.cs_wallet, spk_man->cs_KeyStore);

    // Create a v2 witness prevtx (SegWit format with nTime)
    // Using witness transaction with scriptWitness data
    CDataStream s_prev_tx(ParseHex(REDDCOIN_V2_SEGWIT_TX), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef prev_tx;
    s_prev_tx >> prev_tx;
    BOOST_CHECK(prev_tx->HasWitness());
    m_wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(prev_tx->GetHash()), std::forward_as_tuple(&m_wallet, prev_tx));

    // Add witness script for P2WSH
    CScript ws;
    CDataStream s_ws(ParseHex("47522103089dc10c7ac6db54f91329af617333db388cead0c231f723379d1b99030b02dc21023add904f3d6dcf59ddb906b0dee23529b7ffb9ed50e5e86151926860221f0e7352ae"), SER_NETWORK, PROTOCOL_VERSION);
    s_ws >> ws;
    spk_man->AddCScript(ws);

    // Add redeem script for witness v0 scripthash
    CScript rs;
    CDataStream s_rs(ParseHex("2200208c2353173743b595dfb4a07b72ba8e42e3797da74e87fe7d9d7497e3b2028903"), SER_NETWORK, PROTOCOL_VERSION);
    s_rs >> rs;
    spk_man->AddCScript(rs);

    // Add HD seed
    CKey key = DecodeSecret("7N1r4YVRMcW6AY8qVv5PUKf6MLG9uZfJvVfcsY6T93F2KCcv69u");
    BOOST_REQUIRE(key.IsValid());
    CPubKey master_pub_key = spk_man->DeriveNewSeed(key);
    spk_man->SetHDSeed(master_pub_key);
    spk_man->NewKeyPool();

    // Create PSBT with witness transaction
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nTime = 1600000000;  // v2 has timestamp
    mtx.vin.resize(1);
    mtx.vin[0].prevout.hash = prev_tx->GetHash();
    mtx.vin[0].prevout.n = 1;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 195000000;
    mtx.vout[0].scriptPubKey = GetScriptForDestination(WitnessV0ScriptHash(ws));

    PartiallySignedTransaction psbtx(mtx);

    // Add witness UTXO to PSBT input (required for SegWit)
    psbtx.inputs.resize(1);
    psbtx.inputs[0].witness_utxo = prev_tx->vout[1];
    psbtx.inputs[0].witness_script = ws;
    psbtx.inputs[0].redeem_script = rs;

    // Verify FillPSBT works with SegWit transaction
    bool complete = false;
    TransactionError err = m_wallet.FillPSBT(psbtx, complete, SIGHASH_ALL, false, true);
    BOOST_CHECK_EQUAL(err, TransactionError::OK);

    // Verify witness data is preserved
    BOOST_CHECK(psbtx.tx.has_value());
    BOOST_CHECK(!psbtx.inputs[0].witness_utxo.IsNull());
    BOOST_CHECK(!psbtx.inputs[0].witness_script.empty());

    // Test PSBT serialization preserves witness data
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    PartiallySignedTransaction psbtx_copy;
    ss >> psbtx_copy;
    BOOST_CHECK(!psbtx_copy.inputs[0].witness_utxo.IsNull());
    BOOST_CHECK_EQUAL(psbtx_copy.inputs[0].witness_utxo.nValue, prev_tx->vout[1].nValue);
}

// Test hardcoded Reddcoin transaction vectors
BOOST_AUTO_TEST_CASE(reddcoin_transaction_vectors_test)
{
    // Test v1 PoW transaction deserialization
    CDataStream ss_v1(ParseHex(REDDCOIN_V1_POW_TX), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef tx_v1;
    ss_v1 >> tx_v1;
    BOOST_CHECK_EQUAL(tx_v1->nVersion, 1);
    BOOST_CHECK_EQUAL(tx_v1->nTime, 0);
    BOOST_CHECK_EQUAL(tx_v1->vin.size(), 1);
    BOOST_CHECK_EQUAL(tx_v1->vout.size(), 2);
    BOOST_CHECK_EQUAL(tx_v1->nLockTime, 101);
    BOOST_CHECK(!tx_v1->HasWitness());

    // Test v2 PoS transaction deserialization
    CDataStream ss_v2(ParseHex(REDDCOIN_V2_POS_TX), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef tx_v2;
    ss_v2 >> tx_v2;
    BOOST_CHECK_EQUAL(tx_v2->nVersion, 2);
    BOOST_CHECK(tx_v2->nTime > 0);  // Has timestamp
    BOOST_CHECK_EQUAL(tx_v2->vin.size(), 1);
    BOOST_CHECK_EQUAL(tx_v2->vout.size(), 1);
    BOOST_CHECK(!tx_v2->HasWitness());

    // Test v2 SegWit transaction deserialization
    CDataStream ss_segwit(ParseHex(REDDCOIN_V2_SEGWIT_TX), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef tx_segwit;
    ss_segwit >> tx_segwit;
    BOOST_CHECK_EQUAL(tx_segwit->nVersion, 2);
    BOOST_CHECK(tx_segwit->nTime > 0);
    BOOST_CHECK_EQUAL(tx_segwit->vin.size(), 1);
    BOOST_CHECK_EQUAL(tx_segwit->vout.size(), 2);
    BOOST_CHECK(tx_segwit->HasWitness());
    BOOST_CHECK_EQUAL(tx_segwit->vin[0].scriptWitness.stack.size(), 2);

    // Test redeem script
    auto rs_vec = ParseHex(REDDCOIN_REDEEM_SCRIPT);
    CScript rs(rs_vec.begin(), rs_vec.end());
    BOOST_CHECK(!rs.empty());

    // Test witness script
    auto ws_vec = ParseHex(REDDCOIN_WITNESS_SCRIPT);
    CScript ws(ws_vec.begin(), ws_vec.end());
    BOOST_CHECK(!ws.empty());

    // Test WIF key
    CKey key = DecodeSecret(REDDCOIN_TEST_WIF);
    BOOST_CHECK(key.IsValid());
    BOOST_CHECK(!key.IsCompressed());
}

// Test PSBT with mixed transaction types (ensure compatibility)
BOOST_AUTO_TEST_CASE(psbt_mixed_versions_test)
{
    auto spk_man = m_wallet.GetOrCreateLegacyScriptPubKeyMan();
    LOCK2(m_wallet.cs_wallet, spk_man->cs_KeyStore);

    // Create both v1 and v2 previous transactions
    CDataStream s_prev_tx_v1(ParseHex("0100000001aad73931018bd25f84ae400b68848be09db706eac2ac18298babee71ab656f8b0000000048473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01feffffff0280f0fa020000000017a9140fb9463421696b82c833af241c78c17ddbde493487d0f20a270100000017a91429ca74f8a08f81999428185c97b5d852e4063f618765000000"), SER_NETWORK, PROTOCOL_VERSION);
    CTransactionRef prev_tx_v1;
    s_prev_tx_v1 >> prev_tx_v1;
    m_wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(prev_tx_v1->GetHash()), std::forward_as_tuple(&m_wallet, prev_tx_v1));

    // Create v2 prevtx
    CMutableTransaction prev_mtx_v2;
    prev_mtx_v2.nVersion = 2;
    prev_mtx_v2.nTime = 1600000000;
    prev_mtx_v2.vin.resize(1);
    prev_mtx_v2.vin[0].prevout.hash = uint256S("8b6f65ab71eeba8b2918acc2ea06b79de08b84680b40ae845fd28b0131397ad7");
    {
        auto sig = ParseHex("473044022058f6fc7c6a33e1b31548d481c826c015bd30135aad42cd67790dab66d2ad243b02204a1ced2604c6735b6393e5b41691dd78b00f0c5942fb9f751856faa938157dba01");
        prev_mtx_v2.vin[0].scriptSig = CScript(sig.begin(), sig.end());
    }
    prev_mtx_v2.vout.resize(1);
    prev_mtx_v2.vout[0].nValue = 60000000;
    {
        auto spk = ParseHex("a9140fb9463421696b82c833af241c78c17ddbde493487");
        prev_mtx_v2.vout[0].scriptPubKey = CScript(spk.begin(), spk.end());
    }
    prev_mtx_v2.nLockTime = 101;

    CTransactionRef prev_tx_v2 = MakeTransactionRef(prev_mtx_v2);
    m_wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(prev_tx_v2->GetHash()), std::forward_as_tuple(&m_wallet, prev_tx_v2));

    // Add HD seed
    CKey key = DecodeSecret("7N1r4YVRMcW6AY8qVv5PUKf6MLG9uZfJvVfcsY6T93F2KCcv69u");
    BOOST_REQUIRE(key.IsValid());
    CPubKey master_pub_key = spk_man->DeriveNewSeed(key);
    spk_man->SetHDSeed(master_pub_key);
    spk_man->NewKeyPool();

    // Create PSBT spending from both v1 and v2 inputs
    CMutableTransaction mtx;
    mtx.nVersion = 2;  // Use v2 for output (PoS)
    mtx.nTime = 1600000200;
    mtx.vin.resize(2);
    mtx.vin[0].prevout.hash = prev_tx_v1->GetHash();
    mtx.vin[0].prevout.n = 0;
    mtx.vin[1].prevout.hash = prev_tx_v2->GetHash();
    mtx.vin[1].prevout.n = 0;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 100000000;
    {
        auto spk = ParseHex("76a914d85c2b71d0060b09c9886aeb815e50991dda124d88ac");
        mtx.vout[0].scriptPubKey = CScript(spk.begin(), spk.end());
    }

    PartiallySignedTransaction psbtx(mtx);

    // Verify PSBT handles mixed input versions
    BOOST_CHECK_EQUAL(psbtx.inputs.size(), 2);

    // Test serialization with mixed versions
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    PartiallySignedTransaction psbtx_copy;
    ss >> psbtx_copy;

    BOOST_CHECK(psbtx_copy.tx.has_value());
    BOOST_CHECK_EQUAL(psbtx_copy.tx->nVersion, 2);
    BOOST_CHECK_EQUAL(psbtx_copy.tx->nTime, 1600000200);
    BOOST_CHECK_EQUAL(psbtx_copy.tx->vin.size(), 2);
}

BOOST_AUTO_TEST_CASE(parse_hd_keypath)
{
    std::vector<uint32_t> keypath;

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/1", keypath));
    BOOST_CHECK(!ParseHDKeypath("//////////////////////////'/", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1///////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1/1'/", keypath));
    BOOST_CHECK(!ParseHDKeypath("1/'//////////////////////////", keypath));

    BOOST_CHECK(ParseHDKeypath("", keypath));
    BOOST_CHECK(!ParseHDKeypath(" ", keypath));

    BOOST_CHECK(ParseHDKeypath("0", keypath));
    BOOST_CHECK(!ParseHDKeypath("O", keypath));

    BOOST_CHECK(ParseHDKeypath("0000'/0000'/0000'", keypath));
    BOOST_CHECK(!ParseHDKeypath("0000,/0000,/0000,", keypath));

    BOOST_CHECK(ParseHDKeypath("01234", keypath));
    BOOST_CHECK(!ParseHDKeypath("0x1234", keypath));

    BOOST_CHECK(ParseHDKeypath("1", keypath));
    BOOST_CHECK(!ParseHDKeypath(" 1", keypath));

    BOOST_CHECK(ParseHDKeypath("42", keypath));
    BOOST_CHECK(!ParseHDKeypath("m42", keypath));

    BOOST_CHECK(ParseHDKeypath("4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1

    BOOST_CHECK(ParseHDKeypath("m", keypath));
    BOOST_CHECK(!ParseHDKeypath("n", keypath));

    BOOST_CHECK(ParseHDKeypath("m/", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0''", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0'/0'", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/'0/0'", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("n/0/0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/00", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0/0/f00", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/0/000000000000000000000000000000000000000000000000000000000000000000000000000000000000", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1/1/111111111111111111111111111111111111111111111111111111111111111111111111111111111111", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/00/0", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/0'/00/'0", keypath));

    BOOST_CHECK(ParseHDKeypath("m/1/", keypath));
    BOOST_CHECK(!ParseHDKeypath("m/1//", keypath));

    BOOST_CHECK(ParseHDKeypath("m/0/4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("m/0/4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1

    BOOST_CHECK(ParseHDKeypath("m/4294967295", keypath)); // 4294967295 == 0xFFFFFFFF (uint32_t max)
    BOOST_CHECK(!ParseHDKeypath("m/4294967296", keypath)); // 4294967296 == 0xFFFFFFFF (uint32_t max) + 1
}

BOOST_AUTO_TEST_SUITE_END()
