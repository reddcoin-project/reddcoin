// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_RELEASE_VERIFY_H
#define BITCOIN_NODE_RELEASE_VERIFY_H

#include <fs.h>
#include <node/update_check.h>
#include <uint256.h>

#include <string>

namespace node {
/**
 * The release signing key, x-only BIP340, as specified by REP-1018.
 *
 * This is the trust anchor and it is deliberately compiled in. Verification is
 * entirely local: no keyserver, no handshake, no network lookup of any kind. A
 * hostile network can therefore deny an upgrade but cannot substitute one.
 *
 * Its private half exists only on an air-gapped machine, derived at
 * m/1018'/4'/0'. Rotating this value costs a client release, which was accepted
 * when the alternative was verifying nothing.
 */
extern const std::string RELEASE_PUBKEY;

/**
 * Check a release manifest against the signature published beside it.
 *
 * The message is TaggedHash("Reddcoin/ReleaseManifest") over the manifest bytes
 * exactly as received, per REP-1018 section 3.4. The tag is what stops a
 * signature made here being meaningful in another context, the release key
 * being on the same curve as transaction keys.
 *
 * @param[in]  manifest  The SHA256SUMS bytes, unmodified. Any normalisation
 *                       applied on the way in verifies something the server
 *                       does not serve.
 * @param[in]  signature Contents of SHA256SUMS.sig: 128 hex characters,
 *                       surrounding whitespace tolerated.
 * @param[out] error     Why it failed.
 * @return true only when the signature is good.
 *
 * Rejects a signature that does not decode to exactly 64 bytes before
 * attempting verification, because XOnlyPubKey::VerifySchnorr asserts on any
 * other length and would abort the process rather than return false.
 */
bool VerifyReleaseManifest(const std::string& manifest, const std::string& signature,
                           std::string& error);

/**
 * Find the digest a verified manifest gives for one artifact.
 *
 * @param[in]  manifest The manifest, already verified. Looking a name up in an
 *                      unverified manifest tells you nothing.
 * @param[in]  filename Artifact name, matched against the whole filename field.
 * @param[out] digest   The expected SHA-256, when this returns true.
 * @param[out] error    Why it failed.
 *
 * Requires exactly one matching line. Zero means the release does not contain
 * that file; more than one means the manifest is malformed and the right answer
 * is not knowable.
 *
 * ⚠ The match is on the entire field, never a substring. A published manifest
 * lists signed and unsigned variants side by side, so a loose match for
 * "signed.exe" also matches "win64-setup-unsigned.exe", which would hand a user
 * the unsigned installer while reporting a verified download.
 */
bool FindArtifactDigest(const std::string& manifest, const std::string& filename,
                        uint256& digest, std::string& error);

//! A verified artifact, sitting where the user can be pointed at it.
struct StagedRelease {
    fs::path path;
    std::string filename;
    int64_t size{0};
};

/**
 * Fetch, verify and stage a release artifact.
 *
 * Performs the whole of REP-1018 section 3.6: fetch the manifest and its
 * signature, verify the signature against the compiled-in key, resolve the
 * artifact name to its digest, download the artifact, and hash what arrived.
 * Returns true only when the file on disk matches the digest in a manifest that
 * the release key signed.
 *
 * **Staging layout.** Artifacts live under `<staging_root>/<version>/`, one
 * directory per release. Directories for other versions are removed on success,
 * so at most one staged artifact exists at a time and disk use is bounded by a
 * single release rather than growing with every check.
 *
 * **An already-staged artifact is reused.** If the file is present and hashes
 * to the expected digest it is not downloaded again, so repeating the check
 * costs a manifest fetch rather than 29 MB. A file that is present and does not
 * match is deleted and refetched, since the only thing that can be concluded
 * about it is that it is not the artifact.
 *
 * **A hash mismatch deletes the file.** Leaving it would mean an unverified
 * artifact sitting in the staging directory looking exactly like a verified
 * one.
 *
 * @param[in]  version      Release version, used for the path and the directory.
 * @param[in]  filename     Artifact to fetch, matched exactly in the manifest.
 * @param[in]  staging_root Directory the per-version directories live under.
 * @param[out] out          Where the verified artifact is, on success.
 * @param[out] error        Why it failed. Empty when the caller cancelled.
 */
bool StageVerifiedRelease(const std::string& version, const std::string& filename,
                          const fs::path& staging_root, const DownloadProgress& progress,
                          const DownloadCancel& cancel, StagedRelease& out, std::string& error);

/**
 * SHA-256 of a file on disk, read in blocks rather than loaded.
 *
 * @param[out] digest Set when this returns true.
 * @param[out] error  Why it failed.
 */
bool HashFile(const fs::path& path, uint256& digest, std::string& error);
} // namespace node

#endif // BITCOIN_NODE_RELEASE_VERIFY_H
