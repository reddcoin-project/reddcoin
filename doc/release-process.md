Release Process
====================

## Branch updates

### Before every release candidate

* Update translations see [translation_process.md](https://github.com/reddcoin-project/reddcoin/blob/develop/doc/translation_process.md#synchronising-translations).
* Update release candidate version in `configure.ac` (`CLIENT_VERSION_RC`).
* Update manpages (after rebuilding the binaries), see [gen-manpages.py](https://github.com/reddcoin-project/reddcoin/blob/develop/contrib/devtools/README.md#gen-manpagespy).
* Update reddcoin.conf and commit, see [gen-reddcoin-conf.sh](https://github.com/reddcoin-project/reddcoin/blob/develop/contrib/devtools/README.md#gen-reddcoin-confsh).

### Before every major and minor release

* Update [bips.md](bips.md) to account for changes since the last release (don't forget to bump the version number on the first line).
* Update version in `configure.ac` (don't forget to set `CLIENT_VERSION_RC` to `0`).
* Write release notes (see "Write the release notes" below).

### Before every major release

* On both the master branch and the new release branch:
  - update `CLIENT_VERSION_MAJOR` in [`configure.ac`](../configure.ac)
  - update `CLIENT_VERSION_MAJOR`, `PACKAGE_VERSION`, and `PACKAGE_STRING` in [`build_msvc/bitcoin_config.h`](/build_msvc/bitcoin_config.h)
* On the new release branch in [`configure.ac`](../configure.ac) and [`build_msvc/bitcoin_config.h`](/build_msvc/bitcoin_config.h) (see [this commit](https://github.com/bitcoin/bitcoin/commit/742f7dd)):
  - set `CLIENT_VERSION_MINOR` to `0`
  - set `CLIENT_VERSION_REVISION` to `0`
  - set `CLIENT_VERSION_BUILD` to `0`
  - set `CLIENT_VERSION_IS_RELEASE` to `true`

### The version components

The version is four components, `MAJOR.MINOR.REVISION.BUILD`, e.g. `4.22.9.4`, matching
the scheme Bitcoin Core used historically. Each is defined
in [`configure.ac`](../configure.ac) and mirrored by hand in
[`build_msvc/bitcoin_config.h`](/build_msvc/bitcoin_config.h), which does not run configure.

Two representations exist and must be changed together:

* the **string**, built by `AC_INIT` into `PACKAGE_VERSION`, and
* the **integer** `CLIENT_VERSION` in [`src/clientversion.h`](/src/clientversion.h), weighted
  `1000000*MAJOR + 10000*MINOR + 100*REVISION + 1*BUILD`.

`FormatVersion()` in [`src/clientversion.cpp`](/src/clientversion.cpp) decodes that integer back
into the dotted string used for the peer-visible subversion and the `getnetworkinfo` `subversion`
field. **Its divisors must match the weights above.** A mismatch produces a plausible but wrong
version on the wire, with no compile error; `test_FormatSubVersion` in
[`src/test/util_tests.cpp`](/src/test/util_tests.cpp) is what catches it.

`CLIENT_VERSION` also drives upgrade decisions in `wallet/walletdb.cpp` and
`qt/optionsmodel.cpp`, which compare a previously stored value against the current one. Any
future reweighting must keep newer versions comparing greater than older ones.

#### Before branch-off

* Update hardcoded [seeds](/contrib/seeds/README.md), see [this pull request](https://github.com/bitcoin/bitcoin/pull/7415) for an example.
* Update [`src/chainparams.cpp`](/src/chainparams.cpp) m_assumed_blockchain_size and m_assumed_chain_state_size with the current size plus some overhead (see [this](#how-to-calculate-assumed-blockchain-and-chain-state-size) for information on how to calculate them).
* Update [`src/chainparams.cpp`](/src/chainparams.cpp) chainTxData with statistics about the transaction count and rate. Use the output of the `getchaintxstats` RPC, see
  [this pull request](https://github.com/bitcoin/bitcoin/pull/20263) for an example. Reviewers can verify the results by running `getchaintxstats <window_block_count> <window_final_block_hash>` with the `window_block_count` and `window_final_block_hash` from your output.
* Update `src/chainparams.cpp` nMinimumChainWork and defaultAssumeValid (and the block height comment) with information from the `getblockheader` (and `getblockhash`) RPCs.
  - The selected value must not be orphaned so it may be useful to set the value two blocks back from the tip.
  - Testnet should be set some tens of thousands back from the tip due to reorgs there.
  - This update should be reviewed with a reindex-chainstate with assumevalid=0 to catch any defect
     that causes rejection of blocks in the past history.
- Clear the release notes and move them to the wiki (see "Write the release notes" below).
- Translations on Transifex
    - Create [a new resource](https://www.transifex.com/bitcoin/bitcoin/content/) named after the major version with the slug `[bitcoin.qt-translation-<RRR>x]`, where `RRR` is the major branch number padded with zeros. Use `src/qt/locale/bitcoin_en.xlf` to create it.
    - In the project workflow settings, ensure that [Translation Memory Fill-up](https://docs.transifex.com/translation-memory/enabling-autofill) is enabled and that [Translation Memory Context Matching](https://docs.transifex.com/translation-memory/translation-memory-with-context) is disabled.
    - Update the Transifex slug in [`.tx/config`](/.tx/config) to the slug of the resource created in the first step. This identifies which resource the translations will be synchronized from.
    - Make an announcement that translators can start translating for the new version. You can use one of the [previous announcements](https://www.transifex.com/bitcoin/bitcoin/announcements/) as a template.
    - Change the auto-update URL for the resource to `master`, e.g. `https://raw.githubusercontent.com/bitcoin/bitcoin/master/src/qt/locale/bitcoin_en.xlf`. (Do this only after the previous steps, to prevent an auto-update from interfering.)

#### After branch-off (on the major release branch)

- Update the versions.
- Create a pinned meta-issue for testing the release candidate (see [this issue](https://github.com/bitcoin/bitcoin/issues/17079) for an example) and provide a link to it in the release announcements where useful.
- Translations on Transifex
    - Change the auto-update URL for the new major version's resource away from `master` and to the branch, e.g. `https://raw.githubusercontent.com/bitcoin/bitcoin/<branch>/src/qt/locale/bitcoin_en.xlf`. Do not forget this or it will keep tracking the translations on master instead, drifting away from the specific major release.

#### Before final release

- Merge the release notes from the wiki into the branch.
- Ensure the "Needs release note" label is removed from all relevant pull requests and issues.

#### Tagging a release (candidate)

To tag the version (or release candidate) in git, use the `make-tag.py` script from [bitcoin-maintainer-tools](https://github.com/bitcoin-core/bitcoin-maintainer-tools). From the root of the repository run:

    ../bitcoin-maintainer-tools/make-tag.py v(new version, e.g. 0.20.0)

This will perform a few last-minute consistency checks in the build system files, and if they pass, create a signed tag.

## Building

### First time / New builders

Install Guix using one of the installation methods detailed in
[contrib/guix/INSTALL.md](/contrib/guix/INSTALL.md).

Check out the source code in the following directory hierarchy.

    cd /path/to/your/toplevel/build
    git clone https://github.com/bitcoin-core/guix.sigs.git
    git clone https://github.com/bitcoin-core/bitcoin-detached-sigs.git
    git clone https://github.com/bitcoin/bitcoin.git

### Write the release notes

Open a draft of the release notes for collaborative editing at https://github.com/bitcoin-core/bitcoin-devwiki/wiki.

For the period during which the notes are being edited on the wiki, the version on the branch should be wiped and replaced with a link to the wiki which should be used for all announcements until `-final`.

Generate the change log. As this is a huge amount of work to do manually, there is the `list-pulls` script to do a pre-sorting step based on github PR metadata. See the [documentation in the README.md](https://github.com/bitcoin-core/bitcoin-maintainer-tools/blob/master/README.md#list-pulls).

Generate list of authors:

    git log --format='- %aN' v(current version, e.g. 0.20.0)..v(new version, e.g. 0.20.1) | sort -fiu

### Setup and perform Guix builds

Checkout the Bitcoin Core version you'd like to build:

```sh
pushd ./bitcoin
SIGNER='(your builder key, ie bluematt, sipa, etc)'
VERSION='(new version without v-prefix, e.g. 0.20.0)'
git fetch "v${VERSION}"
git checkout "v${VERSION}"
popd
```

Ensure your guix.sigs are up-to-date if you wish to `guix-verify` your builds
against other `guix-attest` signatures.

```sh
git -C ./guix.sigs pull
```

### Create the macOS SDK tarball: (first time, or when SDK version changes)

Create the macOS SDK tarball, see the [macdeploy
instructions](/contrib/macdeploy/README.md#deterministic-macos-dmg-notes) for
details.

### Build and attest to build outputs:

Follow the relevant Guix README.md sections:
- [Performing a build](/contrib/guix/README.md#performing-a-build)
- [Attesting to build outputs](/contrib/guix/README.md#attesting-to-build-outputs)

### Verify other builders' signatures to your own. (Optional)

Add other builders keys to your gpg keyring, and/or refresh keys: See `../bitcoin/contrib/builder-keys/README.md`.

Follow the relevant Guix README.md sections:
- [Verifying build output attestations](/contrib/guix/README.md#verifying-build-output-attestations)

### Next steps:

Commit your signature to guix.sigs:

```sh
pushd ./guix.sigs
git add "${VERSION}/${SIGNER}"/noncodesigned.SHA256SUMS{,.asc}
git commit -m "Add ${VERSION} unsigned sigs for ${SIGNER}"
git push  # Assuming you can push to the guix.sigs tree
popd
```

Codesigner only: Create Windows/macOS detached signatures:
- Only one person handles codesigning. Everyone else should skip to the next step.
- Only once the Windows/macOS builds each have 3 matching signatures may they be signed with their respective release keys.

Codesigner only: Sign the macOS binary:

    transfer bitcoin-osx-unsigned.tar.gz to macOS for signing
    tar xf bitcoin-osx-unsigned.tar.gz
    ./detached-sig-create.sh -s "Key ID"
    Enter the keychain password and authorize the signature
    Move signature-osx.tar.gz back to the guix-build host

Codesigner only: Sign the windows binaries:

    tar xf bitcoin-win-unsigned.tar.gz
    ./detached-sig-create.sh -key /path/to/codesign.key
    Enter the passphrase for the key when prompted
    signature-win.tar.gz will be created

Code-signer only: It is advised to test that the code signature attaches properly prior to tagging by performing the `guix-codesign` step.
However if this is done, once the release has been tagged in the bitcoin-detached-sigs repo, the `guix-codesign` step must be performed again in order for the guix attestation to be valid when compared against the attestations of non-codesigner builds.

Codesigner only: Commit the detached codesign payloads:

```sh
pushd ./bitcoin-detached-sigs
# checkout the appropriate branch for this release series
rm -rf ./*
tar xf signature-osx.tar.gz
tar xf signature-win.tar.gz
git add -A
git commit -m "point to ${VERSION}"
git tag -s "v${VERSION}" HEAD
git push the current branch and new tag
popd
```

Non-codesigners: wait for Windows/macOS detached signatures:

- Once the Windows/macOS builds each have 3 matching signatures, they will be signed with their respective release keys.
- Detached signatures will then be committed to the [bitcoin-detached-sigs](https://github.com/bitcoin-core/bitcoin-detached-sigs) repository, which can be combined with the unsigned apps to create signed binaries.

Create (and optionally verify) the codesigned outputs:

- [Codesigning](/contrib/guix/README.md#codesigning)

Commit your signature for the signed macOS/Windows binaries:

```sh
pushd ./guix.sigs
git add "${VERSION}/${SIGNER}"/all.SHA256SUMS{,.asc}
git commit -m "Add ${SIGNER} ${VERSION} signed binaries signatures"
git push  # Assuming you can push to the guix.sigs tree
popd
```

### Sign the manifest that will be published

Reddcoin releases are signed by one release manager rather than by three or more
independent builders, so the upstream step of concatenating every signer's
`all.SHA256SUMS.asc` does not apply. Sign the manifest directly instead.

**Sign the exact bytes you are going to publish.** `all.SHA256SUMS` is not that
file. `guix-attest` writes it over *every* fragment, which includes artifacts
that are deliberately not uploaded:

- `reddcoin-${VERSION}-codesignatures-${VERSION}.tar.gz`, an intermediate of the
  codesigning step
- the `*-debug*` archives, which the upload step below excludes on purpose

Listing either in the published manifest leaves entries no one can download, so
`sha256sum -c SHA256SUMS` in the download directory fails on them. Signing
`all.SHA256SUMS` and publishing a different file is worse still: the signature
then reports BAD against what was actually uploaded, which is indistinguishable
from tampering.

The difference is not fixed from release to release either. 4.22.9 shipped
without macOS codesigning, so its `all.SHA256SUMS` happened to match what was
published; 4.22.9.4's did not. Derive the manifest every time rather than
assuming.

Build the manifest and sign it in one place:

```bash
# The release key's full fingerprint, as listed in contrib/builder-keys/keys.txt.
SIGNER_KEY='ABEDC4489B9188E45C2342A82E91240B293BA5D3'

cd guix-build-${VERSION}/output

# The published manifest: only the artifacts that are actually uploaded.
# Nothing under contrib/guix does this filtering. libexec/build.sh builds each
# SHA256SUMS.part from `find "$ACTUAL_OUTDIR" -type f`, so every artifact is
# listed, and guix-attest only concatenates those fragments. That is correct for
# the guix.sigs attestations, which are meant to cover everything that was
# built; it is the published manifest that has to be narrowed, here.
# This exclusion must stay in step with the upload command further down, which
# skips the same files with -not -name "*debug*".
grep -v codesignatures all.SHA256SUMS | grep -v -- '-debug' > SHA256SUMS

gpg --detach-sign --armor --digest-algo sha256 \
    --local-user "$SIGNER_KEY" \
    -o SHA256SUMS.asc SHA256SUMS
```

Verify before uploading anything. This must report a good signature from the
release key:

```bash
gpg --verify SHA256SUMS.asc SHA256SUMS
```

Upload `SHA256SUMS` and `SHA256SUMS.asc` together, and re-upload both if either
is regenerated. A signature uploaded beside an older manifest verifies as BAD,
which is a worse outcome than the missing signature it replaced.

Both files go to the server together. Publishing `SHA256SUMS` without its
signature leaves the manifest authenticated by nothing but TLS to the web host,
which is the state every release up to and including 4.22.9.4 shipped in.

### Sign the manifest for the client

`SHA256SUMS.asc` proves the release to a person with OpenPGP tooling.
`SHA256SUMS.sig` proves it to the client, which has no OpenPGP and, on Windows,
no `gpgv` either. Both cover the same manifest and both are published.

The client verifies against a public key compiled into it, so this signature is
made with a key whose private half **never touches a networked machine**. It
lives on an air-gapped box, derived from a BIP39 mnemonic held offline.

`contrib/release-signing/sign-release-manifest.py` does the signing. It needs
nothing but Python and this repository: the BIP340 implementation and the
official test vectors it uses already live in `test/functional/test_framework`.

#### First, on the air-gapped machine

Prove the crypto works on that machine before it signs anything users will
trust:

```bash
./contrib/release-signing/sign-release-manifest.py selftest
```

That runs the official BIP340 vectors and must report all of them passing.

#### Generate the key, once

Only for the first release signed with a given key, or for a rotation. Skip this
if a key already exists; the public key below tells you whether it does.

```bash
./contrib/release-signing/sign-release-manifest.py generate
```

That prints twenty four words and the public key they derive. Nothing is written
to disk, deliberately: a file holding the mnemonic is a file that gets backed up,
synced, or left on a machine that was supposed to be wiped, and the tool is not
in a position to know which. Write the words on paper.

Entropy comes from the platform CSPRNG. For a key with no expiry you may prefer
not to trust one generator on one machine, in which case supply your own from
dice or coin flips:

```bash
./contrib/release-signing/sign-release-manifest.py generate --entropy-hex "$(cat dice.hex)"
```

That takes 32 bytes as 64 hex characters for a 24-word phrase. 256 dice rolls
recorded in base 6 and converted, or 256 coin flips, both work.

⚠ **Restore the backup before trusting it.** Type the words from the paper copy,
not from the screen, into a scratch file and check they derive the same key:

```bash
./contrib/release-signing/sign-release-manifest.py pubkey --mnemonic /path/to/scratch.txt
```

It must print the public key `generate` showed. A transcription error is caught
here or it is caught years later when the backup is the only copy left. Remove
the scratch file afterwards.

⚠ **Then record the public key in the release process**, in "The release key"
below, on both maintained lines. The client is built with that value compiled in,
so it is the anchor for every verification; a key that exists but is written down
nowhere is not usable by anyone else.

#### Sign

Carry `SHA256SUMS` in, and carry `SHA256SUMS.sig` back out. Nothing else crosses
the gap, and the mnemonic never leaves:

```bash
./contrib/release-signing/sign-release-manifest.py sign SHA256SUMS \
    --mnemonic /path/to/mnemonic.txt
```

A mistyped mnemonic is rejected rather than signed with: the words are checked
against the BIP39 list and the phrase against its own checksum. That matters
most when recovering onto a rebuilt machine, where a wrong word would otherwise
derive a perfectly usable key for a wallet that has never signed anything.

What the checksum cannot tell you is whether a valid phrase is the *right*
phrase, so the tool also prints the public key it signed with. **Check that
against the release key below before publishing.** A signature made with the
wrong key verifies happily against itself and against nothing any released
client will accept, so this is the one check that cannot be skipped.

#### Verify before uploading

On the build host, against the manifest as it will be published:

```bash
./contrib/release-signing/sign-release-manifest.py verify SHA256SUMS SHA256SUMS.sig \
    --pubkey "$RELEASE_PUBKEY"
```

It exits non-zero on a bad signature, so it is safe to gate an upload on.

Upload `SHA256SUMS.sig` beside `SHA256SUMS` and `SHA256SUMS.asc`. The same rule
applies as for the `.asc`: if the manifest is regenerated, **both** signatures
have to be regenerated and re-uploaded with it, or they verify as BAD against
what is served.

#### The release key

```
derivation   m/1018'/4'/0'  (BIP39 mnemonic -> BIP32, all elements hardened)
public key   (fill in when the production key is generated)
```

Deliberately not a wallet path. Reddcoin wallets derive under `m/44'/4'/...`, and
a release key that could collide with a spending key is a bad idea however
unlikely the collision. `4'` is Reddcoin's SLIP-0044 coin type, matching
`nExtCoinType`; the trailing index is the rotation slot, so a replacement key is
`1'` from the same backup rather than a new seed.

Purpose `1018'` had no claimant when it was assigned, checked against the BIP43
registry and against known unregistered users. Its neighbour `1017'` is lnd's,
which derives Lightning keys under `m/1017'/coinType'/keyFamily'/0/index`.

⚠ **The path is part of the key.** A mnemonic does not identify a key without
it, so changing the path after a key exists is changing the key, and every
client carrying the old public key stops verifying. Fix it before generating the
production key, not after.

⚠ **The tag is part of the format.** The signature is over
`TaggedHash("Reddcoin/ReleaseManifest")` of the manifest bytes, matching
`TaggedHash()` in `src/hash.h`. Changing the tag invalidates every signature any
released client will accept.

The public key is compiled into the client, so **rotating it requires a client
release**. That cost was accepted deliberately: any automatic client-side
verification needs a trust anchor, and the alternative was verifying nothing.
Recovery from a lost signing machine is the offline mnemonic backup, not a
delegation chain, so treat that backup with the care a wallet seed gets.

### Publishing the release key

A signature is only useful to someone who can obtain the key that made it. The
fingerprint belongs in [`contrib/builder-keys/keys.txt`](/contrib/builder-keys/keys.txt),
and the key itself has to be fetchable from somewhere the verifier already
trusts. Publish it to both keyservers, since they behave differently and are
consulted by different tools:

```bash
SIGNER_KEY='ABEDC4489B9188E45C2342A82E91240B293BA5D3'

gpg --keyserver hkps://keys.openpgp.org  --send-keys "$SIGNER_KEY"
gpg --keyserver hkps://keyserver.ubuntu.com --send-keys "$SIGNER_KEY"
```

`keys.openpgp.org` accepts the upload immediately but publishes the user ID only
once the address on the key has been confirmed by email, so answer its
verification message. **Until that is done the upload is of no use to anyone.**
The key is served, and a fingerprint lookup returns it, but it comes back with no
user ID attached, and `gpg --import` refuses a key in that state:

```
gpg: Total number processed: 1
gpg:           w/o user IDs: 1
```

so nothing lands in the verifier's keyring. Lookup by email returns 404 as well.
`keys.openpgp.org` also accepts a single user ID and strips all third-party
signatures, distributing only self-signatures. `keyserver.ubuntu.com` performs
none of this validation, accepts whatever it is given, and serves the key with
its user ID straight away, which is the other reason to publish to both.

Confirm both are serving the key before relying on either:

```bash
curl -sf "https://keys.openpgp.org/vks/v1/by-fingerprint/${SIGNER_KEY}" | gpg --show-keys
curl -sf "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x${SIGNER_KEY}" | gpg --show-keys
```

Each should print the fingerprint above with the release manager's user ID. A
200 response alone is not sufficient evidence: a keyserver can answer a lookup
that found nothing with a page that is not a key, so pipe the result through
`gpg --show-keys` rather than checking the status code.

Keep an armoured copy on the download server as well, so verification does not
depend on a keyserver being reachable:

```bash
gpg --export --armor "$SIGNER_KEY" > reddcoin-release-key.asc
```

### Publish the release

- Upload to the download server (`download.reddcoin.com`, `bin/reddcoin-core-${VERSION}/`):
    1. The contents of each `./bitcoin/guix-build-${VERSION}/output/${HOST}/` directory, except for
       `*-debug*` files.

       Guix will output all of the results into host subdirectories, but the SHA256SUMS
       file does not include these subdirectories. In order for downloads via torrent
       to verify without directory structure modification, all of the uploaded files
       need to be in the same directory as the SHA256SUMS file.

       The `*-debug*` files generated by the guix build contain debug symbols
       for troubleshooting by developers. It is assumed that anyone that is
       interested in debugging can run guix to generate the files for
       themselves. To avoid end-user confusion about which file to pick, as well
       as save storage space *do not upload these to the bitcoincore.org server,
       nor put them in the torrent*.

       ```sh
       find guix-build-${VERSION}/output/ -maxdepth 2 -type f -not -name "SHA256SUMS.part" -and -not -name "*debug*" -exec scp {} user@download.reddcoin.com:bin/reddcoin-core-${VERSION} \;
       ```

    2. The `SHA256SUMS` file

    3. The `SHA256SUMS.asc` detached signature you just created and verified

    4. The `SHA256SUMS.sig` Schnorr signature the client verifies

### Verify the published release

Check the upload the way a user will, from the server rather than from the build
tree. This is what catches a manifest that lists files which were never
uploaded, and a signature that was regenerated without its manifest:

```bash
base="https://download.reddcoin.com/bin/reddcoin-core-${VERSION}"
cd "$(mktemp -d)"

curl -sO "$base/SHA256SUMS"
curl -sO "$base/SHA256SUMS.asc"

# Must report a good signature from the release key, against the published
# manifest and not a local copy of it.
gpg --verify SHA256SUMS.asc SHA256SUMS

# Every file the manifest names must be downloadable, and must hash correctly.
# This fetches the whole release, so expect it to take a while.
awk '{print $2}' SHA256SUMS | while read -r f; do curl -sO "$base/$f"; done
sha256sum -c SHA256SUMS
```

`sha256sum -c` has to come out clean with no `--ignore-missing`. Needing that
flag means the manifest lists something that was not uploaded, and a verifier
who sees failures cannot tell a deliberate omission from a corrupted download.

Verifying with a keyring that already trusts the release key proves less than it
appears to. To check what a new user actually experiences, fetch the key from a
keyserver into a throwaway keyring first:

```bash
export GNUPGHOME="$(mktemp -d)"
curl -sf "https://keys.openpgp.org/vks/v1/by-fingerprint/${SIGNER_KEY}" | gpg --import
gpg --verify SHA256SUMS.asc SHA256SUMS
```

A warning that the key is not certified is expected there and is not a failure:
a fresh keyring has no trust path to any key. What matters is `Good signature`
and a primary key fingerprint matching
[`contrib/builder-keys/keys.txt`](/contrib/builder-keys/keys.txt).

### Distribute and announce

- Create a torrent of the `/var/www/bin/bitcoin-core-${VERSION}` directory such
  that at the top level there is only one file: the `bitcoin-core-${VERSION}`
  directory containing everything else. Name the torrent
  `bitcoin-${VERSION}.torrent` (note that there is no `-core-` in this name).

  Optionally help seed this torrent. To get the `magnet:` URI use:

  ```sh
  transmission-show -m <torrent file>
  ```

  Insert the magnet URI into the announcement sent to mailing lists. This permits
  people without access to `bitcoincore.org` to download the binary distribution.
  Also put it into the `optional_magnetlink:` slot in the YAML file for
  bitcoincore.org.

- Update other repositories and websites for new version

  - bitcoincore.org blog post

  - bitcoincore.org maintained versions update:
    [table](https://github.com/bitcoin-core/bitcoincore.org/commits/master/_includes/posts/maintenance-table.md)

  - bitcoincore.org RPC documentation update

      - Install [golang](https://golang.org/doc/install)

      - Install the new Bitcoin Core release

      - Run bitcoind on regtest

      - Clone the [bitcoincore.org repository](https://github.com/bitcoin-core/bitcoincore.org)

      - Run: `go run generate.go` while being in `contrib/doc-gen` folder, and with bitcoin-cli in PATH

      - Add the generated files to git

  - Update packaging repo

      - Push the flatpak to flathub, e.g. https://github.com/flathub/org.bitcoincore.bitcoin-qt/pull/2

      - Push the latest version to master (if applicable), e.g. https://github.com/bitcoin-core/packaging/pull/32

      - Create a new branch for the major release "0.xx" from master (used to build the snap package) and request the
        track (if applicable), e.g. https://forum.snapcraft.io/t/track-request-for-bitcoin-core-snap/10112/7

      - Notify MarcoFalke so that he can start building the snap package

        - https://code.launchpad.net/~bitcoin-core/bitcoin-core-snap/+git/packaging (Click "Import Now" to fetch the branch)
        - https://code.launchpad.net/~bitcoin-core/bitcoin-core-snap/+git/packaging/+ref/0.xx (Click "Create snap package")
        - Name it "bitcoin-core-snap-0.xx"
        - Leave owner and series as-is
        - Select architectures that are compiled via guix
        - Leave "automatically build when branch changes" unticked
        - Tick "automatically upload to store"
        - Put "bitcoin-core" in the registered store package name field
        - Tick the "edge" box
        - Put "0.xx" in the track field
        - Click "create snap package"
        - Click "Request builds" for every new release on this branch (after updating the snapcraft.yml in the branch to reflect the latest guix results)
        - Promote release on https://snapcraft.io/bitcoin-core/releases if it passes sanity checks

  - This repo

      - Archive the release notes for the new version to `doc/release-notes/` (branch `master` and branch of the release)

      - Create a [new GitHub release](https://github.com/bitcoin/bitcoin/releases/new) with a link to the archived release notes

- Announce the release:

  - bitcoin-dev and bitcoin-core-dev mailing list

  - Bitcoin Core announcements list https://bitcoincore.org/en/list/announcements/join/

  - Bitcoin Core Twitter https://twitter.com/bitcoincoreorg

  - Celebrate

### Additional information

#### <a name="how-to-calculate-assumed-blockchain-and-chain-state-size"></a>How to calculate `m_assumed_blockchain_size` and `m_assumed_chain_state_size`

Both variables are used as a guideline for how much space the user needs on their drive in total, not just strictly for the blockchain.
Note that all values should be taken from a **fully synced** node and have an overhead of 5-10% added on top of its base value.

To calculate `m_assumed_blockchain_size`:
- For `mainnet` -> Take the size of the data directory, excluding `/regtest` and `/testnet3` directories.
- For `testnet` -> Take the size of the `/testnet3` directory.


To calculate `m_assumed_chain_state_size`:
- For `mainnet` -> Take the size of the `/chainstate` directory.
- For `testnet` -> Take the size of the `/testnet3/chainstate` directory.

Notes:
- When taking the size for `m_assumed_blockchain_size`, there's no need to exclude the `/chainstate` directory since it's a guideline value and an overhead will be added anyway.
- The expected overhead for growth may change over time, so it may not be the same value as last release; pay attention to that when changing the variables.
