// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_RELEASE_ARTIFACTS_H
#define BITCOIN_NODE_RELEASE_ARTIFACTS_H

#include <string>

namespace node {
/**
 * The release files published for one host.
 *
 * A release publishes a separate build per host, named after the host it was
 * built for, so a running binary can name the file its own machine needs
 * instead of pointing the user at a directory listing.
 */
struct ReleaseArtifacts {
    //! Short platform name as it appears in artifact filenames: "win64",
    //! "osx64", or the normalised host triplet on everything else.
    std::string platform;

    //! The file a reddcoin-qt user should install. An installer on Windows, a
    //! disk image on macOS.
    std::string gui;

    //! The file a reddcoind user should install. An archive everywhere.
    //!
    //! Equal to gui on Linux, where one tarball carries both binaries and no
    //! separate installer is published.
    std::string daemon;
};

/**
 * Name the release artifacts for a host triplet and version.
 *
 * @param[in]  host_triplet An autoconf host triplet, in either the canonical
 *                          four-field form or the three-field alias. See the
 *                          note on normalisation below.
 * @param[in]  version      Release version without a leading "v", for example
 *                          "4.22.9.4".
 * @param[out] out          Filled in only when this returns true.
 * @return false if no build is published for that host, or if either argument
 *         is unusable.
 *
 * **On host triplet forms.** The published Linux artifacts are named after the
 * triplet guix was invoked with, which is the three-field alias
 * (`x86_64-linux-gnu`). `HOST_TRIPLET` holds autoconf's `$host`, which config.sub
 * has canonicalised to four fields (`x86_64-pc-linux-gnu`). The two differ for
 * every Linux host and agree for Windows and macOS, so the vendor field is
 * dropped before a Linux name is built. Both forms are accepted, since a build
 * from source legitimately produces the canonical one.
 */
bool GetReleaseArtifacts(const std::string& host_triplet, const std::string& version, ReleaseArtifacts& out);

/**
 * Name the release artifacts for the host this binary was built for, using the
 * HOST_TRIPLET recorded by configure.
 */
bool GetReleaseArtifactsForThisHost(const std::string& version, ReleaseArtifacts& out);
} // namespace node

#endif // BITCOIN_NODE_RELEASE_ARTIFACTS_H
