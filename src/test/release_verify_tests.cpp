// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/release_verify.h>

#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <string>

using node::FindArtifactDigest;
using node::HashFile;
using node::RELEASE_PUBKEY;
using node::VerifyReleaseManifest;

namespace {
//! A manifest in the shape the release actually publishes, including the signed
//! and unsigned variants that sit side by side. Those pairs are the reason the
//! filename match has to be exact, so they belong in the fixture rather than in
//! one test that remembers to add them.
const std::string MANIFEST{
    "94e9735251606649b8087c5e7af93e26265bd742bbcf688dd87b22401514807f  reddcoin-4.22.9.4-aarch64-linux-gnu.tar.gz\n"
    "05a818023af2a88af6dde102820df5c5c2655935ca1a8f7b707bc7214dc13293  reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz\n"
    "1111111111111111111111111111111111111111111111111111111111111111  reddcoin-4.22.9.4-win64-setup-signed.exe\n"
    "2222222222222222222222222222222222222222222222222222222222222222  reddcoin-4.22.9.4-win64-setup-unsigned.exe\n"
    "3333333333333333333333333333333333333333333333333333333333333333  reddcoin-4.22.9.4-osx-signed.dmg\n"
    "4444444444444444444444444444444444444444444444444444444444444444  reddcoin-4.22.9.4-osx-unsigned.dmg\n"};

std::string HexOf(const uint256& value)
{
    return HexStr(Span<const unsigned char>(value.begin(), value.size()));
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(release_verify_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(the_pinned_key_is_usable)
{
    // A well formed 32 bytes need not be a point on the curve, and an anchor
    // that is not one would fail at the first verification rather than here.
    const std::vector<unsigned char> bytes{ParseHex(RELEASE_PUBKEY)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    BOOST_CHECK_EQUAL(RELEASE_PUBKEY.size(), 64U);
    BOOST_CHECK(RELEASE_PUBKEY == ToLower(RELEASE_PUBKEY));

    const XOnlyPubKey pubkey{bytes};
    BOOST_CHECK(pubkey.IsFullyValid());
}

BOOST_AUTO_TEST_CASE(an_exact_filename_is_required)
{
    // The one that matters. A substring match for "signed.exe" also matches
    // "win64-setup-unsigned.exe", which would hand a Windows user the unsigned
    // installer while reporting a verified download.
    uint256 digest;
    std::string error;

    BOOST_REQUIRE(FindArtifactDigest(MANIFEST, "reddcoin-4.22.9.4-win64-setup-signed.exe", digest, error));
    BOOST_CHECK_EQUAL(HexOf(digest),
                      "1111111111111111111111111111111111111111111111111111111111111111");

    BOOST_REQUIRE(FindArtifactDigest(MANIFEST, "reddcoin-4.22.9.4-win64-setup-unsigned.exe", digest, error));
    BOOST_CHECK_EQUAL(HexOf(digest),
                      "2222222222222222222222222222222222222222222222222222222222222222");

    // A partial name resolves to nothing rather than to whichever line happens
    // to contain it.
    BOOST_CHECK(!FindArtifactDigest(MANIFEST, "signed.exe", digest, error));
    BOOST_CHECK(!FindArtifactDigest(MANIFEST, "win64-setup-signed.exe", digest, error));
    BOOST_CHECK(!FindArtifactDigest(MANIFEST, "reddcoin-4.22.9.4-win64-setup-signed", digest, error));
}

BOOST_AUTO_TEST_CASE(a_name_the_manifest_does_not_list_fails)
{
    uint256 digest;
    std::string error;
    BOOST_CHECK(!FindArtifactDigest(MANIFEST, "reddcoin-4.22.9.4-riscv64-linux-gnu.tar.gz", digest, error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!FindArtifactDigest(MANIFEST, "", digest, error));
}

BOOST_AUTO_TEST_CASE(a_duplicated_entry_fails_rather_than_picking_one)
{
    // Which digest is correct is precisely what a duplicate makes unknowable,
    // so taking either would be inventing an answer.
    const std::string duplicated{
        MANIFEST +
        "5555555555555555555555555555555555555555555555555555555555555555  reddcoin-4.22.9.4-osx-signed.dmg\n"};

    uint256 digest;
    std::string error;
    BOOST_CHECK(!FindArtifactDigest(duplicated, "reddcoin-4.22.9.4-osx-signed.dmg", digest, error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(malformed_lines_are_skipped_not_matched)
{
    const std::string messy{
        "\n"
        "not a manifest line at all\n"
        "zzzz  reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz\n"
        "05a818023af2a88af6dde102820df5c5c2655935ca1a8f7b707bc7214dc13293 one-space-only.tar.gz\n" +
        MANIFEST};

    uint256 digest;
    std::string error;
    // The good line is still found, and the malformed ones matched nothing.
    BOOST_REQUIRE(FindArtifactDigest(messy, "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz", digest, error));
    BOOST_CHECK_EQUAL(HexOf(digest),
                      "05a818023af2a88af6dde102820df5c5c2655935ca1a8f7b707bc7214dc13293");
    BOOST_CHECK(!FindArtifactDigest(messy, "one-space-only.tar.gz", digest, error));
}

BOOST_AUTO_TEST_CASE(a_signature_of_the_wrong_length_is_rejected_before_verifying)
{
    // Load bearing rather than defensive. XOnlyPubKey::VerifySchnorr asserts on
    // any length other than 64, so reaching it with a malformed signature file
    // would abort the process instead of reporting a bad release.
    std::string error;
    BOOST_CHECK(!VerifyReleaseManifest(MANIFEST, std::string(126, 'a'), error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!VerifyReleaseManifest(MANIFEST, std::string(130, 'a'), error));
    BOOST_CHECK(!VerifyReleaseManifest(MANIFEST, "", error));
    BOOST_CHECK(!VerifyReleaseManifest(MANIFEST, "not hexadecimal at all", error));
}

BOOST_AUTO_TEST_CASE(a_signature_from_another_key_is_rejected)
{
    // Structurally valid, correct length, and not ours.
    std::string error;
    BOOST_CHECK(!VerifyReleaseManifest(MANIFEST, std::string(128, '0'), error));
    BOOST_CHECK_EQUAL(error, "Release manifest signature is not valid");
}

BOOST_AUTO_TEST_CASE(hashing_a_file_matches_the_manifest_form)
{
    // The comparison phase 4b makes: a file on disk against a digest read out
    // of a verified manifest.
    const fs::path path{gArgs.GetDataDirNet() / "artifact"};
    {
        fsbridge::ofstream out{path, std::ios::binary};
        out << "reddcoin";
    }

    uint256 digest;
    std::string error;
    BOOST_REQUIRE(HashFile(path, digest, error));
    // sha256("reddcoin")
    BOOST_CHECK_EQUAL(HexOf(digest),
                      "827ea7b9e26989931cbb1f10711d6575ad01aeac043607044d7eb09f322d0d8c");
}

BOOST_AUTO_TEST_CASE(hashing_a_missing_file_fails_cleanly)
{
    uint256 digest;
    std::string error;
    BOOST_CHECK(!HashFile(gArgs.GetDataDirNet() / "no-such-file", digest, error));
    BOOST_CHECK(!error.empty());
}


BOOST_AUTO_TEST_CASE(the_real_published_release_verifies)
{
    // The published 4.22.9.4 manifest and its signature, byte for byte as the
    // server serves them. This is the check that the pinned key, the tag, the
    // message construction and the parser all agree with what the release
    // process actually produced, rather than merely with each other.
    //
    // Offline on purpose: a regression test, not a liveness check.
    const std::string published{
        "94e9735251606649b8087c5e7af93e26265bd742bbcf688dd87b22401514807f  reddcoin-4.22.9.4-aarch64-linux-gnu.tar.gz\n"
        "a61fb2624553e85bc4b7353a45cb0b633f7e943efe298e741985788b9e6553f9  reddcoin-4.22.9.4-arm-linux-gnueabihf.tar.gz\n"
        "e7930ad5fd11f43f5a0c472a502ccee95af9a78f82507f9795b4e64d05daf19b  reddcoin-4.22.9.4.tar.gz\n"
        "5f57deca7d27419e7774f2058ca7ea15a8179a70d4bc131dcba741486cc8baa3  reddcoin-4.22.9.4-powerpc64-linux-gnu.tar.gz\n"
        "c856d963da6332cfae8301742c6ccd9900042445002666204661a79fcd6a5fc4  reddcoin-4.22.9.4-powerpc64le-linux-gnu.tar.gz\n"
        "aeaf2ef8605f23e5bc59f26863b25a5bda56f8b11a427e2854f7506231a0f56f  reddcoin-4.22.9.4-riscv64-linux-gnu.tar.gz\n"
        "e9a9d13701bc6bb010848c346306abf103e5bb67a68fc86a914f2538b96ca4c8  reddcoin-4.22.9.4-osx-signed.dmg\n"
        "ffa2120b392df5f4abae316d835d07091b221b11692bff7ff433edaa772c31a5  reddcoin-4.22.9.4-osx-unsigned.dmg\n"
        "25d9156bbcfc624b92fc29af4260854de8d35464bd1eeea2859f4ba2ddfd5dfa  reddcoin-4.22.9.4-osx-unsigned.tar.gz\n"
        "ea169121d668952a26ef8c25b4db2f9831e255452da72d424d0be8d4fca11f51  reddcoin-4.22.9.4-osx64.tar.gz\n"
        "05a818023af2a88af6dde102820df5c5c2655935ca1a8f7b707bc7214dc13293  reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz\n"
        "96dbc096e6ff41d0fa34051fc5b1fb9ec09eabd5a79e850c5042c1692a27c85a  reddcoin-4.22.9.4-win64-setup-signed.exe\n"
        "9a44ebfa6dfcf21027b1132d6482cc96d6806faf4e8d695379e8f6bfb947fdc5  reddcoin-4.22.9.4-win-unsigned.tar.gz\n"
        "541e1e48598fdc68bb59ed0f845390c63a18e32f871212d42cb8f53a0abd7f1f  reddcoin-4.22.9.4-win64-setup-unsigned.exe\n"
        "facc901ddf2ce860df9b0e1d2fe21b723a7283e1dc0487694bcdde938d597dc6  reddcoin-4.22.9.4-win64.zip\n"};
    const std::string signature{
        "67b1904a0fee2f3e313124b5ddb513356db7e46285f6fe2f044a6453e3364e39"
        "b991765a99c423c85a14f612bf29fbd37abfcd9abc7e67220188cae1fbcdc648"};

    std::string error;
    BOOST_CHECK_MESSAGE(VerifyReleaseManifest(published, signature, error),
                        "the real release failed to verify: " << error);

    // And the digest it gives for this host's artifact is the one downloading
    // that artifact actually produced.
    uint256 digest;
    BOOST_REQUIRE(FindArtifactDigest(published, "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz",
                                     digest, error));
    BOOST_CHECK_EQUAL(HexOf(digest),
                      "05a818023af2a88af6dde102820df5c5c2655935ca1a8f7b707bc7214dc13293");

    // One byte different is a different release, which shows the signature is
    // bound to these bytes rather than verifying for some other reason.
    std::string tampered{published};
    tampered[0] = tampered[0] == '9' ? '8' : '9';
    BOOST_CHECK(!VerifyReleaseManifest(tampered, signature, error));

    // As does an appended line: the realistic corruption is a manifest
    // regenerated and republished without being re-signed.
    BOOST_CHECK(!VerifyReleaseManifest(
        published + "0000000000000000000000000000000000000000000000000000000000000000  extra\n",
        signature, error));
}

BOOST_AUTO_TEST_SUITE_END()
