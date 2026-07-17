// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Created by ROSHii on 2019-06-01.
//

#include <base58.h>
#include <key.h>
#include <key_io.h>
#include <test/data/bip39_vectors.json.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/bip32.h>
#include <util/bip39.h>

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

	// Skip keys that are not a supported language name. The
	// "japanese_ideographic" variant uses the ideographic space (U+3000)
	// between words and cannot be round-tripped through FromData (which
	// emits ASCII spaces); it is exercised by the dedicated
	// bip39_japanese_ideographic_space test below.
	if (nLanguage == -1) {
	    continue;
	}

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

// UTF-8 encoding of the ideographic space (U+3000, "　"), which BIP39 uses to
// separate words in Japanese mnemonics.
static const std::string IDEOGRAPHIC_SPACE = "\xe3\x80\x80";

// Replace every ideographic space (U+3000) in the given string with an ASCII
// space, yielding the equivalent phrase a user might paste from a wallet that
// substitutes the ASCII separator.
static std::string ToAsciiSpaces(const std::string& s)
{
    std::string out = s;
    std::string::size_type pos = 0;
    while ((pos = out.find(IDEOGRAPHIC_SPACE, pos)) != std::string::npos) {
        out.replace(pos, IDEOGRAPHIC_SPACE.size(), " ");
        pos += 1;
    }
    return out;
}

// Build a phrase whose separators alternate between ideographic and ASCII
// spaces, mimicking a mnemonic assembled from inconsistently-separated sources.
static SecureString ToMixedSpaces(const std::string& asciiPhrase)
{
    std::string out;
    bool ideographic = true;
    for (char c : asciiPhrase) {
        if (c == ' ') {
            out += ideographic ? IDEOGRAPHIC_SPACE : std::string(" ");
            ideographic = !ideographic;
        } else {
            out += c;
        }
    }
    return SecureString(out.begin(), out.end());
}

// The canonical BIP39 Japanese vectors, imported verbatim from upstream
// (trezor/python-mnemonic), separate words with the ideographic space (U+3000).
// Wallets and users frequently substitute the ASCII space (U+0020). Exercise
// that the authentic ideographic form, its ASCII-space equivalent, and a mix of
// the two all validate and derive the same, correct seed.
BOOST_AUTO_TEST_CASE(bip39_japanese_ideographic_space)
{
    UniValue json;
    std::string json_data(json_tests::bip39_vectors,
                          json_tests::bip39_vectors + sizeof(json_tests::bip39_vectors));

    if (!json.read(json_data) || !json.isObject()) {
        BOOST_ERROR("Parse error.");
        return;
    }

    int nLanguage = CMnemonic::getLanguageIndex("japanese");
    BOOST_REQUIRE(nLanguage != -1);

    UniValue tests = find_value(json.get_obj(), "japanese_ideographic").get_array();
    BOOST_REQUIRE(tests.size() > 0);

    SecureString passphrase("TREZOR");

    for (unsigned int i = 0; i < tests.size(); i++) {
        UniValue test = tests[i].get_array();

        // test[1] is the authentic upstream mnemonic, separated by U+3000.
        std::string strIdeographic = test[1].get_str();
        std::string strAscii = ToAsciiSpaces(strIdeographic);

        SecureString ideographicMnemonic(strIdeographic.begin(), strIdeographic.end());
        SecureString asciiMnemonic(strAscii.begin(), strAscii.end());
        SecureString mixedMnemonic = ToMixedSpaces(strAscii);

        // Sanity check: the vector genuinely uses ideographic spaces, and the
        // ASCII variant differs byte-for-byte.
        BOOST_CHECK(strIdeographic.find(IDEOGRAPHIC_SPACE) != std::string::npos);
        BOOST_CHECK(ideographicMnemonic != asciiMnemonic);

        // All three separator styles must validate, both with the language
        // supplied explicitly and via auto-detection.
        BOOST_CHECK(CMnemonic::Check(ideographicMnemonic, nLanguage));
        BOOST_CHECK(CMnemonic::Check(mixedMnemonic, nLanguage));
        BOOST_CHECK(CMnemonic::Check(ideographicMnemonic));
        BOOST_CHECK(CMnemonic::Check(mixedMnemonic));

        // Word count / strength must be computed from the normalized words, not
        // fooled into seeing a single run-on word.
        BOOST_CHECK_EQUAL(CMnemonic::getWordCount(asciiMnemonic),
                          CMnemonic::getWordCount(ideographicMnemonic));
        BOOST_CHECK_EQUAL(CMnemonic::getWordCount(asciiMnemonic),
                          CMnemonic::getWordCount(mixedMnemonic));
        BOOST_CHECK_EQUAL(CMnemonic::getStrength(asciiMnemonic),
                          CMnemonic::getStrength(ideographicMnemonic));

        // Language auto-detection must resolve to Japanese for every style.
        BOOST_CHECK_EQUAL(CMnemonic::DetectLanguageSeed(ideographicMnemonic), nLanguage);
        BOOST_CHECK_EQUAL(CMnemonic::DetectLanguageSeed(mixedMnemonic), nLanguage);

        // The derived seed must be identical regardless of the space character,
        // and must match the canonical vector seed.
        SecureVector asciiSeed, ideographicSeed, mixedSeed;
        CMnemonic::ToSeed(asciiMnemonic, passphrase, asciiSeed);
        CMnemonic::ToSeed(ideographicMnemonic, passphrase, ideographicSeed);
        CMnemonic::ToSeed(mixedMnemonic, passphrase, mixedSeed);

        BOOST_CHECK(HexStr(asciiSeed) == test[2].get_str());
        BOOST_CHECK(ideographicSeed == asciiSeed);
        BOOST_CHECK(mixedSeed == asciiSeed);
    }
}

BOOST_AUTO_TEST_SUITE_END()
