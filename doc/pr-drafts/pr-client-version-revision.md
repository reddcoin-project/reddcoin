# build: add a fourth version component, CLIENT_VERSION_REVISION

The version carried three components, so `configure.ac` produced `4.22.9` and `CLIENT_VERSION` encoded as `10000*MAJOR + 100*MINOR + BUILD` = 42209.

The fourth digit in tags like `v4.22.9.3` existed **only in the tag name**. `share/genbuild.sh` sets `BUILD_GIT_TAG` for a clean tagged build and `clientversion.cpp` prefers it over `PACKAGE_VERSION`, so the banner showed the tag while `getnetworkinfo` reported `42209` and the peer subversion stayed `/Reddoshi:4.22.9/`, indistinguishable from 4.22.9 itself. A point release could not identify itself to the network, which is why the compat tests have to fingerprint builds by git commit.

Two commits: the version component, then a parsing fix it makes reachable.

## 1. The version component

Adopts the four-component scheme Bitcoin Core used historically, `MAJOR.MINOR.REVISION.BUILD`, so the layout matches upstream and future merges do not have to reconcile a different ordering. What was `BUILD` becomes `REVISION`, keeping its value of 9, and `BUILD` becomes the new trailing component. It is **0 on develop**; the release branch sets it to the point-release number.

The integer is reweighted to `1000000*MAJOR + 10000*MINOR + 100*REVISION + 1*BUILD`, and `FormatVersion()` widened to decode four components.

### Why the encoding needed care

`CLIENT_VERSION` is not only cosmetic. It drives real upgrade decisions:

* `wallet/walletdb.cpp` rewrites the stored version when the loaded one is older
* `qt/optionsmodel.cpp` migrates QSettings on the same comparison
* `policy/fees.cpp` compares a required version on load

So the encoding has to stay ordered above the old scheme. It does: the old encoding could not reach 100000 for a single-digit major, while the new one starts at 4000000.

Downgrade compatibility was checked rather than assumed:

* `policy/fees.cpp` gates `fee_estimates.dat` on a hardcoded `42199` literal, not `CLIENT_VERSION`
* wallet readability is gated on `FEATURE_LATEST`, a separate constant family

Confirmed empirically by creating a wallet with a patched build and reopening it with an unpatched one: `walletversion 169900`, keypool intact.

### Keeping encode and decode in step

`FormatVersion()` feeds the peer-visible subversion and the `getnetworkinfo` subversion field. A mismatch with the weights in `clientversion.h` produces a plausible but wrong version on the wire, with no compile error. Both sites carry a comment saying so, and `test_FormatSubVersion` asserts a known integer renders its expected four-component string, so a future divergence fails the build.

### Incidental fix

The reweighting aligns `CLIENT_VERSION` with the four-field decoder that `test/functional/test_framework/test_framework.py` has always used in `get_bin_from_version()`. The three-field encoding never matched it. That was latent breakage waiting for the first person to put a Reddcoin version into a `versions=[...]` list.

### Surfaces updated

`configure.ac`, `src/clientversion.h`, `src/clientversion.cpp`, all six Windows `.rc` files, `share/qt/Info.plist.in`, `share/setup.nsi.in`, `build_msvc/bitcoin_config.h`, the cppcheck lint macro list, and `doc/release-process.md`.

Windows `VERSIONINFO` already reserved a fourth field that `rc.exe` was zero-padding, and `setup.nsi.in` carried a literal `.0` in the same slot; both now take the full four components. `build_msvc/bitcoin_config.h` is hand-maintained because MSVC never runs configure, and was stale at 4.22.6; it is corrected as well as extended.

## 2. Update-check parsing

`CheckForUpdates()` matched versions with an **unanchored** `regex_search` for exactly three components. On a four-component string the engine slides the start position until the trailing `$` can match, dropping the major:

| Input | Captured |
|---|---|
| `4.22.9` | `4.22.9` |
| `4.22.9.4` | **`22.9.4`** |
| `v4.22.9.3` | **`22.9.3`** |

The result is silent rather than loud. semver parses `22.9.4` happily, and it compares greater than any real release, so `remoteV > localV` is false and `remoteV == localV` is false: no update offered, no message shown. **A node would never learn a newer release exists.**

This is **pre-existing**. `v4.22.9.3` is already a published tag and arrives as `tag_name` from the GitHub API, so it parses as `22.9.3` today. Adding a fourth component to `PACKAGE_VERSION` extends the same fault to the local side, which is why the fix rides along here.

The expression is anchored, the separators escaped, and the fourth component optional. semver has no fourth field, so it continues to order the first three plus any prerelease tag, and the build number breaks ties. `4.22.9` and `4.22.9.0` stay equal, `4.22.9.4` sorts above both, `4.22.10` above all of them.

Unrecognised input now throws with the offending string. Previously the return value of `regex_search` was discarded, so a failed match fell through to `semver::parse("")` and surfaced as a bare "Invalid version" with no clue what had been read.

## Testing

* Full unit suite: 488 cases, no errors
* `feature_uacomment.py`, `feature_includeconf.py`, `interface_bitcoin_cli.py` all pass. The first is the sharp one: it slices the subversion string from the end, and the version got longer
* Runtime: `"version": 4220900`, `"subversion": "/Reddoshi:4.22.9.0/"`, `protocolversion` unchanged at 80016
* `checkupdates` against the live GitHub API, exercising the mixed case of a four-component local against a three-component remote tag:

```
"localversion": "4.22.9.0",
"remoteversion": "4.22.9",
"updateavailable": false,
"message": "You're running the most recent version of Reddcoin Core (4.22.9.0)",
"officialDownloadLink": "https://download.reddcoin.com/bin/reddcoin-core-4.22.9"
```

Correctly judged equal, and the download link keeps the numeric-only shape that `download.reddcoin.com` expects. Under the old code that call produced no message at all.

* The four-component parsing was also checked against a 14-case matrix covering three- and four-component input, prerelease ordering, and malformed strings

## Compatibility note for reviewers

`getnetworkinfo`'s `version` field changes shape, from `42209` to `4220900`. Anything downstream parsing it as a fixed-width five-digit number needs to accommodate that. The P2P handshake version is `PROTOCOL_VERSION` and is untouched.

One inherited behaviour changes: `qt/optionsmodel.cpp` compares the stored settings version against a literal `130000` from upstream, which was permanently true under the old encoding and is now false. That migration targeted a Bitcoin Core 0.13 dbcache default and was never meaningful here.

## Backport

The 4.22.9 line needs the same change with `CLIENT_VERSION_BUILD` set to 4. Opened separately against `v4.22.9-regtest`. The update-check fix has to be applied to `src/rpc/server.cpp` there, since the `node::CheckForUpdates` refactor is not on that branch; the parsing block itself is identical.
