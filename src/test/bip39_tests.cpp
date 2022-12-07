//
// Created by ROSHii on 2019-06-01.
//

#include <base58.h>
#include <key.h>
#include <key_io.h>
#include <test/data/bip39_vectors.json.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <wallet/bip39.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

// In script_tests.cpp
extern UniValue read_json(const std::string& jsondata);

BOOST_FIXTURE_TEST_SUITE(bip39_tests, BasicTestingSetup)

// https://github.com/trezor/python-mnemonic/blob/b502451a33a440783926e04428115e0bed87d01f/vectors.json
BOOST_AUTO_TEST_CASE(bip39_vectors)
{
    UniValue json;
    std::string json_data(json_tests::bip39_vectors,
                          json_tests::bip39_vectors + sizeof(json_tests::bip39_vectors));

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
	    CExtPubKey pubkey;

	    key.SetSeed(&seed[0], 64);
	    pubkey = key.Neuter();

	    // printf("CBitcoinExtKey: %s\n", EncodeExtKey(key).c_str());
	    BOOST_CHECK(EncodeExtKey(key) == test[3].get_str());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
