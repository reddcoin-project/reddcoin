// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/release_artifacts.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

using node::GetReleaseArtifacts;
using node::ReleaseArtifacts;

namespace {
//! The release the expectations below were taken from. Every filename in this
//! file appears in the published SHA256SUMS for it, so if the guix naming
//! scheme changes these tests fail rather than quietly encoding a stale
//! convention.
const std::string VERSION{"4.22.9.4"};

ReleaseArtifacts Resolve(const std::string& host)
{
    ReleaseArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(GetReleaseArtifacts(host, VERSION, artifacts),
                          "no artifacts resolved for host " << host);
    return artifacts;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(release_artifacts_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(windows_offers_the_installer_to_the_gui)
{
    const ReleaseArtifacts a{Resolve("x86_64-w64-mingw32")};
    BOOST_CHECK_EQUAL(a.platform, "win64");
    // The installer is the artifact that carries an Authenticode signature; the
    // zip is what a reddcoind user wants.
    BOOST_CHECK_EQUAL(a.gui, "reddcoin-4.22.9.4-win64-setup-signed.exe");
    BOOST_CHECK_EQUAL(a.daemon, "reddcoin-4.22.9.4-win64.zip");
}

BOOST_AUTO_TEST_CASE(macos_offers_the_signed_dmg_to_the_gui)
{
    const ReleaseArtifacts a{Resolve("x86_64-apple-darwin18")};
    BOOST_CHECK_EQUAL(a.platform, "osx64");
    BOOST_CHECK_EQUAL(a.gui, "reddcoin-4.22.9.4-osx-signed.dmg");
    BOOST_CHECK_EQUAL(a.daemon, "reddcoin-4.22.9.4-osx64.tar.gz");
}

BOOST_AUTO_TEST_CASE(linux_serves_one_tarball_to_both)
{
    // No separate installer is published for Linux: a single tarball carries
    // both binaries, so the two must resolve to the same file rather than the
    // gui entry being left empty.
    const ReleaseArtifacts a{Resolve("x86_64-linux-gnu")};
    BOOST_CHECK_EQUAL(a.platform, "x86_64-linux-gnu");
    BOOST_CHECK_EQUAL(a.gui, "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz");
    BOOST_CHECK_EQUAL(a.daemon, a.gui);
}

BOOST_AUTO_TEST_CASE(every_published_host_resolves)
{
    // The guix HOSTS list, in the alias form the artifact names are built from.
    // Each expected filename appears in the published 4.22.9.4 SHA256SUMS.
    const struct { const char* host; const char* daemon; } cases[]{
        {"x86_64-linux-gnu", "reddcoin-4.22.9.4-x86_64-linux-gnu.tar.gz"},
        {"arm-linux-gnueabihf", "reddcoin-4.22.9.4-arm-linux-gnueabihf.tar.gz"},
        {"aarch64-linux-gnu", "reddcoin-4.22.9.4-aarch64-linux-gnu.tar.gz"},
        {"riscv64-linux-gnu", "reddcoin-4.22.9.4-riscv64-linux-gnu.tar.gz"},
        {"powerpc64-linux-gnu", "reddcoin-4.22.9.4-powerpc64-linux-gnu.tar.gz"},
        {"powerpc64le-linux-gnu", "reddcoin-4.22.9.4-powerpc64le-linux-gnu.tar.gz"},
        {"x86_64-w64-mingw32", "reddcoin-4.22.9.4-win64.zip"},
        {"x86_64-apple-darwin18", "reddcoin-4.22.9.4-osx64.tar.gz"},
    };

    for (const auto& c : cases) {
        BOOST_CHECK_EQUAL(Resolve(c.host).daemon, c.daemon);
    }
}

BOOST_AUTO_TEST_CASE(canonical_and_alias_triplets_agree)
{
    // The regression this guards. configure records autoconf's $host, which
    // config.sub has expanded to four fields, while the artifacts are named
    // after the three-field alias guix was invoked with. Keying the mapping on
    // the canonical form without normalising would name
    // reddcoin-4.22.9.4-x86_64-pc-linux-gnu.tar.gz, which does not exist.
    const struct { const char* canonical; const char* alias; } pairs[]{
        {"x86_64-pc-linux-gnu", "x86_64-linux-gnu"},
        {"arm-unknown-linux-gnueabihf", "arm-linux-gnueabihf"},
        {"aarch64-unknown-linux-gnu", "aarch64-linux-gnu"},
        {"riscv64-unknown-linux-gnu", "riscv64-linux-gnu"},
        {"powerpc64-unknown-linux-gnu", "powerpc64-linux-gnu"},
        {"powerpc64le-unknown-linux-gnu", "powerpc64le-linux-gnu"},
    };

    for (const auto& p : pairs) {
        const ReleaseArtifacts from_canonical{Resolve(p.canonical)};
        const ReleaseArtifacts from_alias{Resolve(p.alias)};
        BOOST_CHECK_EQUAL(from_canonical.platform, from_alias.platform);
        BOOST_CHECK_EQUAL(from_canonical.gui, from_alias.gui);
        BOOST_CHECK_EQUAL(from_canonical.daemon, from_alias.daemon);
        // And specifically that the vendor is gone, not merely consistent.
        BOOST_CHECK(from_canonical.gui.find("-pc-") == std::string::npos);
        BOOST_CHECK(from_canonical.gui.find("-unknown-") == std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(windows_and_macos_are_not_normalised)
{
    // Their vendor fields are load bearing: x86_64-w64-mingw32 and
    // x86_64-apple-darwin18 are the forms guix uses, so stripping the vendor
    // from them would be wrong. They are matched by OS, not rebuilt from
    // fields, but assert it so a change to NormaliseTriplet cannot leak in.
    BOOST_CHECK_EQUAL(Resolve("x86_64-w64-mingw32").platform, "win64");
    BOOST_CHECK_EQUAL(Resolve("x86_64-apple-darwin18").platform, "osx64");
}

BOOST_AUTO_TEST_CASE(unpublished_hosts_resolve_to_nothing)
{
    // Naming a file that will 404 is worse than saying nothing.
    ReleaseArtifacts artifacts;
    for (const char* host : {"sparc-sun-solaris2.11", "x86_64-unknown-freebsd13", "wasm32-unknown-emscripten"}) {
        BOOST_CHECK_MESSAGE(!GetReleaseArtifacts(host, VERSION, artifacts),
                            "unexpectedly resolved artifacts for " << host);
    }
}

BOOST_AUTO_TEST_CASE(empty_arguments_are_rejected)
{
    ReleaseArtifacts artifacts;
    BOOST_CHECK(!GetReleaseArtifacts("", VERSION, artifacts));
    BOOST_CHECK(!GetReleaseArtifacts("x86_64-linux-gnu", "", artifacts));
}

BOOST_AUTO_TEST_CASE(version_is_interpolated_not_assumed)
{
    ReleaseArtifacts artifacts;
    BOOST_REQUIRE(GetReleaseArtifacts("x86_64-w64-mingw32", "5.0.0", artifacts));
    BOOST_CHECK_EQUAL(artifacts.gui, "reddcoin-5.0.0-win64-setup-signed.exe");
}

BOOST_AUTO_TEST_SUITE_END()
