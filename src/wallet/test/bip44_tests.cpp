// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Created by ROSHii on 2019-06-01.
//

#include <base58.h>
#include <key.h>
#include <key_io.h>
#include <test/data/bip44_vectors.json.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/bip32.h>
#include <wallet/bip39.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

// In script_tests.cpp
extern UniValue read_json(const std::string& jsondata);
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;

BOOST_FIXTURE_TEST_SUITE(bip44_tests, BasicTestingSetup)

// https://github.com/trezor/python-mnemonic/blob/b502451a33a440783926e04428115e0bed87d01f/vectors.json
BOOST_AUTO_TEST_CASE(bip44_vectors)
{
    UniValue json;
    std::string json_data(json_tests::bip44_vectors,
                          json_tests::bip44_vectors + sizeof(json_tests::bip44_vectors));

    if (!json.read(json_data) || !json.isObject()) {
        BOOST_ERROR("Parse error.");
        return;
    }

    std::vector<std::string> keys = json.getKeys();
    for (unsigned int i = 0; i < keys.size(); i++) {

	int nLanguage = CMnemonic::getLanguageIndex(keys.at(i).c_str());

	// printf("Lang = %s (%i)\n", keys.at(i).c_str(), nLanguage);

	UniValue tests = find_value(json.get_obj(), keys.at(i).c_str()).get_array();
	// printf("Number of tests %li\n", tests.size());

	std::string strTest = tests.write();
	if (tests.size() < 4) // Allow for extra stuff (useful for comments)
	{
	    BOOST_ERROR("Bad test: " << strTest.c_str());
	    continue;
	}

	for (unsigned int i = 0; i < tests.size(); i++) {
	    UniValue test = tests[i].get_array();

	    std::vector<uint8_t> vData = ParseHex(test[0].get_str());
	    SecureVector data(vData.begin(), vData.end());

	    SecureString m = CMnemonic::FromData(data, data.size(), nLanguage);
	    std::string strMnemonic = test[1].get_str();
	    SecureString mnemonic(strMnemonic.begin(), strMnemonic.end());

	    // printf("%s\n%s\n", m.c_str(), mnemonic.c_str());

	    BOOST_CHECK(m == mnemonic);
	    BOOST_CHECK(CMnemonic::Check(mnemonic, nLanguage));

	    SecureVector seed;
	    SecureString passphrase("TREZOR");
	    CMnemonic::ToSeed(mnemonic, passphrase, seed);
	    // printf("seed: %s\n", HexStr(seed).c_str());
	    BOOST_CHECK(HexStr(seed) == test[2].get_str());

	    CExtKey key;
	    CExtKey purposeKey;
	    CExtKey coinTypeKey;
	    CExtKey accountKey;
	    CExtKey chainChildKey;
	    CExtPubKey pubkey;

	    int nAccountIndex = 0;
	    bool internal = false;

	    key.SetSeed(&seed[0], 64);
	    pubkey = key.Neuter();

	    // printf("CBitcoinExtKey: %s\n", EncodeExtKey(key).c_str());
	    BOOST_CHECK(EncodeExtKey(key) == test[3].get_str());

	    std::vector<uint32_t> path;

	    // printf("Path: %s\n", test[9].get_str().c_str());

            if (!ParseHDKeypath(test[8].get_str(), path)) {
        	BOOST_ERROR("Error reading wallet database: keymeta with invalid HD keypath");
                return;
            }

            // printf("Path index 0 0x%08x\n", path[0]);

            if (path[0] != (44 | 0x80000000)) {
        	BOOST_ERROR("Unexpected path index of " << path[0] << " (expected 0x8000002c) for the element at index 0");
		return;
	    }

            // printf("Path index 1 0x%08x\n", path[1]);

            if (path[1] != (Params().ExtCoinType() | 0x80000000)) {
        	BOOST_ERROR("Unexpected path index of " << path[1] << " (expected 0x80000004) for the element at index 1");
		return;
	    }

            // printf("Path index 2 0x%08x\n", path[2]);

            if (path[2] != (0x80000000)) {
		BOOST_ERROR("Unexpected path index of " << path[2] << " (expected 0x80000000) for the element at index 2");
		return;
	    }

            // printf("Path index 3 0x%08x\n", path[3]);

	    if (path[3] != (0x00000000)) {
		BOOST_ERROR("Unexpected path index of " << path[3] << " (expected 0x00000000) for the element at index 3");
		return;
	    }

	    // derive m/purpose'
	    key.Derive(purposeKey, 44 | BIP32_HARDENED_KEY_LIMIT);

            // derive m/purpose'/coin_type'
            purposeKey.Derive(coinTypeKey, 4 | BIP32_HARDENED_KEY_LIMIT);

            // derive m/purpose'/coin_type'/account'
	    coinTypeKey.Derive(accountKey, nAccountIndex | BIP32_HARDENED_KEY_LIMIT);


	    // printf("accountExtKey: %s\n", test[8].get_str().c_str());
	    BOOST_CHECK(EncodeExtKey(accountKey) == test[7].get_str());

	    // derive m/purpose'/coin_type'/account'/index
	    accountKey.Derive(chainChildKey, (internal ? 1 : 0));

	    // printf("chainExtKey: %s\n", test[10].get_str().c_str());
	    BOOST_CHECK(EncodeExtKey(chainChildKey) == test[9].get_str());

        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
