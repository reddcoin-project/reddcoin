// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/release_verify.h>

#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <span.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>

#include <array>
#include <string>
#include <vector>

namespace {
//! Domain separation for a release signature, per REP-1018 section 3.4.
//!
//! The release key sits on the same curve as transaction keys, so a signature
//! over a bare SHA256 could be meaningful in another context. The tag is part
//! of the format: changing a byte of it invalidates every signature any
//! released client will accept.
const CHashWriter HASHER_RELEASE_MANIFEST = TaggedHash("Reddcoin/ReleaseManifest");

//! The message a release signature commits to.
uint256 ManifestHash(const std::string& manifest)
{
    CHashWriter hasher{HASHER_RELEASE_MANIFEST};
    hasher.write(manifest.data(), manifest.size());
    return hasher.GetSHA256();
}

//! Split a manifest line into its digest and filename fields.
//!
//! A line is "<64 hex>  <filename>", two spaces, as sha256sum writes it. The
//! filename is taken as the entire remainder of the line so a name containing
//! spaces is matched whole rather than truncated at the first one.
bool SplitManifestLine(const std::string& line, std::string& hex, std::string& filename)
{
    if (line.size() < 67) return false;
    hex = line.substr(0, 64);
    if (line.compare(64, 2, "  ") != 0) return false;
    if (!IsHex(hex)) return false;
    filename = line.substr(66);
    return !filename.empty();
}
} // namespace

// Recorded in doc/release-process.md on both maintained lines. Generated on the
// air-gapped machine and its backup restored and confirmed before being written
// down anywhere.
const std::string node::RELEASE_PUBKEY{
    "23e6b696f2b69cd753de0d8fe3875e989085fbb6f2dc09026ba8ba1df33585db"};

bool node::VerifyReleaseManifest(const std::string& manifest, const std::string& signature,
                                 std::string& error)
{
    error.clear();

    const std::string trimmed{TrimString(signature)};
    if (!IsHex(trimmed)) {
        error = "Release signature is not hexadecimal";
        return false;
    }
    const std::vector<unsigned char> sig{ParseHex(trimmed)};

    // Mandatory, and before VerifySchnorr rather than inside it: that function
    // asserts on any length other than 64, so a malformed signature file would
    // abort the process instead of being rejected.
    if (sig.size() != 64) {
        error = "Release signature is " + ToString(sig.size()) + " bytes, expected 64";
        return false;
    }

    const std::vector<unsigned char> key_bytes{ParseHex(RELEASE_PUBKEY)};
    if (key_bytes.size() != 32) {
        error = "Release public key is malformed";
        return false;
    }
    const XOnlyPubKey pubkey{key_bytes};
    if (!pubkey.IsFullyValid()) {
        error = "Release public key is not a valid point";
        return false;
    }

    if (!pubkey.VerifySchnorr(ManifestHash(manifest), sig)) {
        error = "Release manifest signature is not valid";
        return false;
    }
    return true;
}

bool node::FindArtifactDigest(const std::string& manifest, const std::string& filename,
                              uint256& digest, std::string& error)
{
    error.clear();
    if (filename.empty()) {
        error = "No artifact name to look up";
        return false;
    }

    std::string found_hex;
    int matches{0};
    for (std::string::size_type start{0}; start <= manifest.size();) {
        const std::string::size_type eol{manifest.find('\n', start)};
        const std::string raw{manifest.substr(start, eol == std::string::npos ? std::string::npos
                                                                             : eol - start)};
        start = (eol == std::string::npos) ? manifest.size() + 1 : eol + 1;

        // Tolerate CRLF and stray whitespace. What the signature covered is the
        // manifest bytes; how the lines are terminated is not the question here.
        const std::string line{TrimString(raw)};
        if (line.empty()) continue;

        std::string hex;
        std::string name;
        if (!SplitManifestLine(line, hex, name)) continue;

        // Whole field, never a substring. The manifest lists signed and
        // unsigned variants side by side, so a loose match for "signed.exe"
        // also matches "win64-setup-unsigned.exe" and would hand the user the
        // unsigned installer while reporting a verified download.
        if (name != filename) continue;

        ++matches;
        found_hex = hex;
    }

    if (matches == 0) {
        error = "The release manifest does not list " + filename;
        return false;
    }
    if (matches > 1) {
        // Not recoverable by picking one. Which digest is correct is exactly
        // what a duplicated entry makes unknowable.
        error = "The release manifest lists " + filename + " " + ToString(matches) + " times";
        return false;
    }

    digest = uint256{};
    const std::vector<unsigned char> bytes{ParseHex(found_hex)};
    if (bytes.size() != 32) {
        error = "The release manifest has a malformed digest for " + filename;
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), digest.begin());
    return true;
}

namespace {
//! Remove every staged directory except the one for this version.
//!
//! Called after a success rather than before the download, so a failed fetch
//! never destroys a good artifact that is already staged.
void PruneOtherVersions(const fs::path& staging_root, const std::string& keep)
{
    if (!fs::is_directory(staging_root)) return;
    for (fs::directory_iterator it{staging_root}; it != fs::directory_iterator{}; ++it) {
        const fs::path entry{it->path()};
        if (!fs::is_directory(entry)) continue;
        if (entry.filename().string() == keep) continue;

        // Best effort. A directory that cannot be removed, because a file in it
        // is open on Windows for instance, costs disk rather than correctness.
        fs::remove_all(entry);
    }
}
} // namespace

bool node::StageVerifiedRelease(const std::string& version, const std::string& filename,
                                const fs::path& staging_root, const DownloadProgress& progress,
                                const DownloadCancel& cancel, StagedRelease& out,
                                std::string& error)
{
    error.clear();
    if (version.empty() || filename.empty()) {
        error = "No release to stage";
        return false;
    }

    const std::string release_dir{"reddcoin-core-" + version};
    const std::string base{"/bin/" + release_dir};

    // The manifest and its signature are small and have to be held whole to be
    // checked, so they go through the in-memory fetch rather than to disk.
    std::string manifest;
    if (!FetchToString(RELEASE_DOWNLOAD_HOST, base + "/SHA256SUMS", manifest, error)) {
        error = "Could not fetch the release manifest: " + error;
        return false;
    }
    std::string signature;
    if (!FetchToString(RELEASE_DOWNLOAD_HOST, base + "/SHA256SUMS.sig", signature, error)) {
        error = "Could not fetch the release signature: " + error;
        return false;
    }

    // Nothing below this line trusts the manifest until it has been verified,
    // and nothing above it trusts anything at all.
    if (!VerifyReleaseManifest(manifest, signature, error)) return false;

    uint256 expected;
    if (!FindArtifactDigest(manifest, filename, expected, error)) return false;

    const fs::path version_dir{staging_root / release_dir};
    const fs::path artifact{version_dir / filename};

    // An artifact already staged and already correct is not fetched again, so
    // repeating the check costs a manifest rather than the whole release.
    bool have_it{false};
    if (fs::exists(artifact)) {
        uint256 present;
        std::string ignored;
        have_it = HashFile(artifact, present, ignored) && present == expected;
        if (!have_it) {
            // The only thing knowable about it is that it is not the artifact.
            fs::remove(artifact);
        }
    }

    if (!have_it) {
        fs::create_directories(version_dir);
        if (!DownloadToFile(RELEASE_DOWNLOAD_HOST, base + "/" + filename, artifact, progress,
                            cancel, error)) {
            // A cancelled download leaves error empty, and that is carried
            // through rather than turned into a failure message.
            return false;
        }

        uint256 got;
        if (!HashFile(artifact, got, error)) {
            fs::remove(artifact);
            return false;
        }
        if (got != expected) {
            // Deleted rather than kept. An unverified artifact left in the
            // staging directory is indistinguishable from a verified one.
            fs::remove(artifact);
            error = "The downloaded file does not match the signed manifest";
            return false;
        }
    }

    PruneOtherVersions(staging_root, release_dir);

    out.path = artifact;
    out.filename = filename;
    out.size = static_cast<int64_t>(fs::file_size(artifact));
    return true;
}

bool node::HashFile(const fs::path& path, uint256& digest, std::string& error)
{
    error.clear();

    fsbridge::ifstream file{path, std::ios::binary};
    if (!file.good()) {
        error = "Could not open " + path.string();
        return false;
    }

    CSHA256 hasher;
    std::array<char, 64 * 1024> buffer{};
    while (file.good()) {
        file.read(buffer.data(), buffer.size());
        const std::streamsize got{file.gcount()};
        if (got > 0) {
            hasher.Write(reinterpret_cast<const unsigned char*>(buffer.data()),
                         static_cast<size_t>(got));
        }
        if (got == 0) break;
    }
    if (file.bad()) {
        error = "Could not read " + path.string();
        return false;
    }

    hasher.Finalize(digest.begin());
    return true;
}
