// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <node/release_artifacts.h>

#include <string>
#include <vector>

namespace {
//! Split a host triplet on '-'.
std::vector<std::string> SplitTriplet(const std::string& host_triplet)
{
    std::vector<std::string> fields;
    std::string::size_type start{0};
    while (true) {
        const std::string::size_type dash{host_triplet.find('-', start)};
        if (dash == std::string::npos) {
            fields.push_back(host_triplet.substr(start));
            return fields;
        }
        fields.push_back(host_triplet.substr(start, dash - start));
        start = dash + 1;
    }
}

//! Reduce a host triplet to the form the release artifacts are named after.
//!
//! guix is invoked with the three-field alias, so that is what ends up in the
//! filename, while configure records the four-field form config.sub expands it
//! to. Dropping the vendor turns one into the other:
//!
//!   x86_64-pc-linux-gnu           -> x86_64-linux-gnu
//!   arm-unknown-linux-gnueabihf   -> arm-linux-gnueabihf
//!
//! A triplet that is already in the alias form is returned unchanged, so a
//! build configured either way names the same file.
std::string NormaliseTriplet(const std::string& host_triplet)
{
    const std::vector<std::string> fields{SplitTriplet(host_triplet)};
    if (fields.size() < 4) return host_triplet;

    std::string normalised{fields[0]};
    for (std::vector<std::string>::size_type i = 2; i < fields.size(); ++i) {
        normalised += "-" + fields[i];
    }
    return normalised;
}

//! Does host_triplet name this operating system? Matched on the whole field
//! rather than as a substring, so an architecture that happens to contain the
//! name cannot be mistaken for it.
bool HostIs(const std::string& host_triplet, const std::string& os_prefix)
{
    for (const std::string& field : SplitTriplet(host_triplet)) {
        if (field.compare(0, os_prefix.size(), os_prefix) == 0) return true;
    }
    return false;
}
} // namespace

bool node::GetReleaseArtifacts(const std::string& host_triplet, const std::string& version, ReleaseArtifacts& out)
{
    if (host_triplet.empty() || version.empty()) return false;

    // Every artifact is named after the release, then the platform.
    const std::string prefix{"reddcoin-" + version + "-"};

    if (HostIs(host_triplet, "mingw")) {
        // A reddcoin-qt user wants the installer, which is the artifact that
        // carries an Authenticode signature. A reddcoind user wants the archive.
        out.platform = "win64";
        out.gui = prefix + "win64-setup-signed.exe";
        out.daemon = prefix + "win64.zip";
        return true;
    }

    if (HostIs(host_triplet, "darwin")) {
        // Only an x86_64 build is published. An arm64 Mac runs it under Rosetta
        // and needs no special case here: HOST_TRIPLET is fixed at build time,
        // so a Mac running this binary is by definition running the x86_64 one
        // and already reports x86_64-apple-darwin18. Naming it as the Intel
        // build is the presentation layer's job.
        out.platform = "osx64";
        out.gui = prefix + "osx-signed.dmg";
        out.daemon = prefix + "osx64.tar.gz";
        return true;
    }

    if (HostIs(host_triplet, "linux")) {
        // One tarball carries both binaries; no separate installer is published,
        // because the install shape on Linux is not knowable from here.
        out.platform = NormaliseTriplet(host_triplet);
        out.gui = prefix + out.platform + ".tar.gz";
        out.daemon = out.gui;
        return true;
    }

    // A host nobody publishes a build for. Saying nothing is better than naming
    // a file that will 404.
    return false;
}

std::string node::HostTriplet()
{
#if defined(HOST_TRIPLET)
    return HOST_TRIPLET;
#else
    return "";
#endif
}

bool node::GetReleaseArtifactsForThisHost(const std::string& version, ReleaseArtifacts& out)
{
#if defined(HOST_TRIPLET)
    return GetReleaseArtifacts(HOST_TRIPLET, version, out);
#else
    (void)version;
    (void)out;
    return false;
#endif
}
