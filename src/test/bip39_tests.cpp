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

#include <utf8proc.h>

#include <cstdlib>
#include <string>

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

	// Skip keys that are not a supported language name (e.g. the
	// "japanese_nfkd" vectors, which carry a per-vector passphrase and are
	// exercised by the dedicated bip39_nfkd_passphrase test below).
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

// The canonical BIP39 Japanese vectors separate words with the ideographic
// space (U+3000), but wallets and users frequently substitute the ASCII space
// (U+0020). Exercise that the ideographic form, its ASCII-space equivalent, and
// a mix of the two all validate and derive the same, correct seed.
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

    UniValue tests = find_value(json.get_obj(), "japanese").get_array();
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

// Compose a UTF-8 string to Unicode NFC, mimicking mnemonic text as most
// platforms and input methods emit it. The shipped wordlists are stored in
// NFKD, so NFC input only matches once CMnemonic normalizes it.
static std::string ToNFC(const std::string& s)
{
    utf8proc_uint8_t* out = utf8proc_NFC(reinterpret_cast<const utf8proc_uint8_t*>(s.c_str()));
    if (out == nullptr) {
        return s;
    }
    std::string result(reinterpret_cast<char*>(out));
    free(out);
    return result;
}

// A valid mnemonic pasted in NFC form (the platform default) must validate and
// derive the same seed as its stored NFKD form. The French, Spanish and Korean
// wordlists are stored decomposed (NFKD), so without normalization an NFC phrase
// fails validation outright; this is the headline defect the NFKD change fixes.
BOOST_AUTO_TEST_CASE(bip39_nfc_input)
{
    UniValue json;
    std::string json_data(json_tests::bip39_vectors,
                          json_tests::bip39_vectors + sizeof(json_tests::bip39_vectors));
    BOOST_REQUIRE(json.read(json_data) && json.isObject());

    SecureString passphrase("TREZOR");
    const char* langs[] = {"french", "spanish", "korean"};

    for (const char* lang : langs) {
        int nLanguage = CMnemonic::getLanguageIndex(lang);
        BOOST_REQUIRE(nLanguage != -1);
        UniValue tests = find_value(json.get_obj(), lang).get_array();
        BOOST_REQUIRE(tests.size() > 0);

        bool sawComposedDifference = false;
        for (unsigned int i = 0; i < tests.size(); i++) {
            UniValue test = tests[i].get_array();
            std::string strNfkd = test[1].get_str();   // stored wordlist form (NFKD)
            std::string strNfc = ToNFC(strNfkd);       // as a user would paste it

            if (strNfc != strNfkd) {
                sawComposedDifference = true;
            }

            SecureString nfcMnemonic(strNfc.begin(), strNfc.end());
            BOOST_CHECK(CMnemonic::Check(nfcMnemonic, nLanguage));

            SecureVector seed;
            CMnemonic::ToSeed(nfcMnemonic, passphrase, seed);
            BOOST_CHECK(HexStr(seed) == test[2].get_str());
        }

        // The NFC form must actually differ from the stored NFKD form for some
        // vectors, otherwise the test would pass without exercising the fix.
        BOOST_CHECK_MESSAGE(sawComposedDifference,
                            std::string("NFC == NFKD for every ") + lang + " vector");
    }
}

// The canonical BIP39 Japanese NFKD test vectors (bip32JP / test_JP_BIP39):
// NFC-composed mnemonics with a passphrase containing compatibility characters
// (e.g. U+338D SQUARE ME, which NFKD-decomposes to "メートル"). They exercise
// NFKD of both the mnemonic and the passphrase.
// Format: [entropy, mnemonic(U+3000, NFC), passphrase, seed, xprv].
BOOST_AUTO_TEST_CASE(bip39_nfkd_passphrase)
{
    UniValue json;
    std::string json_data(json_tests::bip39_vectors,
                          json_tests::bip39_vectors + sizeof(json_tests::bip39_vectors));
    BOOST_REQUIRE(json.read(json_data) && json.isObject());

    int nLanguage = CMnemonic::getLanguageIndex("japanese");
    BOOST_REQUIRE(nLanguage != -1);

    UniValue tests = find_value(json.get_obj(), "japanese_nfkd").get_array();
    BOOST_REQUIRE(tests.size() > 0);

    for (unsigned int i = 0; i < tests.size(); i++) {
        UniValue test = tests[i].get_array();
        std::string strMnemonic = test[1].get_str();
        std::string strPassphrase = test[2].get_str();

        SecureString mnemonic(strMnemonic.begin(), strMnemonic.end());
        SecureString passphrase(strPassphrase.begin(), strPassphrase.end());

        // NFC mnemonic with ideographic spaces must validate.
        BOOST_CHECK(CMnemonic::Check(mnemonic, nLanguage));

        // The seed matches the vector only when the passphrase is NFKD-normalized
        // too (the passphrase contains compatibility characters).
        SecureVector seed;
        CMnemonic::ToSeed(mnemonic, passphrase, seed);
        BOOST_CHECK(HexStr(seed) == test[3].get_str());
    }
}

// Malformed UTF-8 must degrade gracefully rather than crash: NormalizeNFKD
// returns the input unchanged on a utf8proc error, so Check reports invalid and
// ToSeed still produces a 64-byte seed.
BOOST_AUTO_TEST_CASE(bip39_malformed_utf8)
{
    std::string bad;
    for (int i = 0; i < 12; i++) {
        if (i) bad += ' ';
        bad += "\xff\xfe"; // invalid UTF-8
    }
    SecureString mnemonic(bad.begin(), bad.end());
    SecureString passphrase("\xff");

    BOOST_CHECK(!CMnemonic::Check(mnemonic));

    SecureVector seed;
    BOOST_CHECK_NO_THROW(CMnemonic::ToSeed(mnemonic, passphrase, seed));
    BOOST_CHECK_EQUAL(seed.size(), 64U);
}

BOOST_AUTO_TEST_SUITE_END()
