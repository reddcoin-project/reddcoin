// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <pubkey.h>
#include <span.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

//! Agreement with the signing tool, on the values REP-1018 publishes.
//!
//! The release signature is produced by a Python tool on an air-gapped machine
//! and checked by this code. Nothing else forces the two to agree, and a
//! disagreement would not show up until every release failed to verify for
//! every user. These are the vectors from REP-1018 section 5.2, so a change on
//! either side breaks this file.

namespace {
//! REP-1018 section 5.2. Two lines, two spaces between digest and name, each
//! line terminated. Signed exactly as published, with no normalisation.
const std::string MANIFEST{
    "1f0b3ca2d0f4bd0b04ef3f0e8a3ab88a1e1b0f7a8e1d5a3b6c9d2e4f0a1b2c3d  reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz\n"
    "9e8d7c6b5a4938271605f4e3d2c1b0a9f8e7d6c5b4a39281706f5e4d3c2b1a09  reddcoin-4.22.9.4-win64.zip\n"};

//! The tag is part of the format. Changing it invalidates every signature any
//! released client will accept, so it is spelled out here rather than shared
//! with the implementation: this file is meant to fail if the tag moves.
const std::string TAG{"Reddcoin/ReleaseManifest"};

const std::string EXPECTED_TAGGED_HASH{"f0b197e423efe74b8898f0a7d3e51404c20beb817caf2f86de5182741dcaf480"};
const std::string EXPECTED_BARE_SHA256{"4d363d6853b51e5734c41b13bbf6bcb16c16d8c437e1d96c270dde3a02fd080a"};
const std::string RELEASE_PUBKEY{"53c9cbcba0ac673c841e72aa4a430206110feba034df81c58fba3917a80f6700"};
const std::string RELEASE_SIG{
    "d552158b18f0f68ee2c67404b70475baac9f324aaa7870c1735f6a1e7c146b03"
    "28f7f000651917afab0e8393841985c3a1013f527b1e17a4dfe2c0d2f14f4608"};

//! Raw byte order, matching what the tool prints. uint256::ToString() reverses,
//! which would silently compare against a byte-swapped value.
std::string RawHex(const uint256& value)
{
    return HexStr(Span<const unsigned char>(value.begin(), value.size()));
}

//! The message a release signature commits to: TaggedHash(tag) over the
//! manifest bytes. This is the whole of REP-1018 section 3.4.
uint256 ManifestHash(const std::string& tag, const std::string& manifest)
{
    CHashWriter hasher{TaggedHash(tag)};
    hasher.write(manifest.data(), manifest.size());
    return hasher.GetSHA256();
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(release_manifest_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(the_manifest_vector_is_intact)
{
    // Guards the vector itself. A transcription slip here would make every
    // other case in this file agree with the wrong thing.
    BOOST_CHECK_EQUAL(MANIFEST.size(), 202U);
}

BOOST_AUTO_TEST_CASE(cpp_and_the_signing_tool_agree_on_the_message)
{
    // The link nothing else checks. The Python tool computed this value; if
    // TaggedHash() here disagrees, no release we sign will ever verify.
    BOOST_CHECK_EQUAL(RawHex(ManifestHash(TAG, MANIFEST)), EXPECTED_TAGGED_HASH);
}

BOOST_AUTO_TEST_CASE(domain_separation_is_applied_not_merely_described)
{
    // A signature over a bare SHA256 of the manifest could be meaningful in
    // another protocol context, which is the whole reason for the tag. Assert
    // the two differ, so an implementation that quietly drops the tagged hash
    // fails here rather than in production.
    const uint256 tagged{ManifestHash(TAG, MANIFEST)};

    uint256 bare;
    CSHA256()
        .Write(reinterpret_cast<const unsigned char*>(MANIFEST.data()), MANIFEST.size())
        .Finalize(bare.begin());

    BOOST_CHECK_EQUAL(RawHex(bare), EXPECTED_BARE_SHA256);
    BOOST_CHECK(tagged != bare);
}

BOOST_AUTO_TEST_CASE(the_reference_signature_verifies)
{
    // End to end through the exact path a client will use: a signature made by
    // the offline tool, checked by XOnlyPubKey::VerifySchnorr.
    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    const std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};
    BOOST_REQUIRE_EQUAL(key_bytes.size(), 32U);
    BOOST_REQUIRE_EQUAL(sig.size(), 64U);

    const XOnlyPubKey pubkey{key_bytes};
    BOOST_REQUIRE(pubkey.IsFullyValid());
    BOOST_CHECK(pubkey.VerifySchnorr(ManifestHash(TAG, MANIFEST), sig));
}

BOOST_AUTO_TEST_CASE(a_modified_manifest_fails)
{
    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    const std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};
    const XOnlyPubKey pubkey{key_bytes};

    std::string tampered{MANIFEST};
    tampered[0] = (tampered[0] == '1') ? '2' : '1';

    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash(TAG, tampered), sig));
}

BOOST_AUTO_TEST_CASE(an_appended_line_fails)
{
    // The realistic failure: the manifest is regenerated and re-uploaded but
    // the signature is not, so what is served no longer matches what was signed.
    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    const std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};
    const XOnlyPubKey pubkey{key_bytes};

    const std::string regenerated{
        MANIFEST + "0000000000000000000000000000000000000000000000000000000000000000  extra.tar.gz\n"};

    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash(TAG, regenerated), sig));
}

BOOST_AUTO_TEST_CASE(a_modified_signature_fails)
{
    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    const XOnlyPubKey pubkey{key_bytes};

    std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};
    sig[0] ^= 0x01;

    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash(TAG, MANIFEST), sig));
}

BOOST_AUTO_TEST_CASE(another_key_does_not_verify)
{
    const std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};

    std::vector<unsigned char> other{ParseHex(RELEASE_PUBKEY)};
    other[0] ^= 0x01;
    const XOnlyPubKey pubkey{other};

    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash(TAG, MANIFEST), sig));
}

BOOST_AUTO_TEST_CASE(the_tag_is_load_bearing)
{
    // One character short of the real tag, then one character long. If either
    // verified, the tag would be decoration and there would be no domain
    // separation at all.
    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    const std::vector<unsigned char> sig{ParseHex(RELEASE_SIG)};
    const XOnlyPubKey pubkey{key_bytes};

    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash("Reddcoin/ReleaseManifes", MANIFEST), sig));
    BOOST_CHECK(!pubkey.VerifySchnorr(ManifestHash("Reddcoin/ReleaseManifest ", MANIFEST), sig));
}

// Note: VerifySchnorr asserts sigbytes.size() == 64, so a malformed signature
// aborts rather than returning false. That length check is the caller's job and
// belongs in the client code REP-1018 section 3.6 step 2 requires; it cannot be
// exercised here without killing the test binary.

BOOST_AUTO_TEST_SUITE_END()
