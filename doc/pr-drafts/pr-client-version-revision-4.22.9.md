# build: add a fourth version component, CLIENT_VERSION_REVISION

Backports the four-component version scheme to the 4.22.9 line, with `CLIENT_VERSION_BUILD` set to **4** so the coming point release identifies itself as `4.22.9.4`. Based on `v4.22.9-regtest` at `8e0eea737a`.

Develop side is #487, where the same change lands with `BUILD` at 0.

## Why this matters here specifically

The fourth digit in `v4.22.9.3` existed **only in the tag name**. `share/genbuild.sh` sets `BUILD_GIT_TAG` for a clean tagged build and `clientversion.cpp` prefers it over `PACKAGE_VERSION`, so the banner showed the tag while `getnetworkinfo` reported `42209` and the peer subversion stayed `/Reddoshi:4.22.9/` - indistinguishable from 4.22.9, 4.22.9.1 and 4.22.9.2.

This is the branch that actually ships point releases, so it is the branch where a version that cannot identify itself does real harm. It is also why the compat tests currently fingerprint builds by git commit rather than by version string.

Two commits: the version component, then a parsing fix it makes reachable.

## 1. The version component

Adopts the four-component scheme Bitcoin Core used historically, `MAJOR.MINOR.REVISION.BUILD`, so the layout matches upstream. What was `BUILD` becomes `REVISION`, keeping its value of 9, and `BUILD` becomes the new trailing component, set to 4 here.

The integer is reweighted to `1000000*MAJOR + 10000*MINOR + 100*REVISION + 1*BUILD`, giving **4220904**, and `FormatVersion()` widened to decode four components.

### Why the encoding needed care

`CLIENT_VERSION` is not only cosmetic. `wallet/walletdb.cpp` rewrites the stored version when the loaded one is older, `qt/optionsmodel.cpp` migrates QSettings on the same comparison, and `policy/fees.cpp` compares a required version on load. The encoding therefore has to stay ordered above the old scheme, and it does: the old encoding could not reach 100000 for a single-digit major, while the new one starts at 4000000.

Downgrade compatibility was checked rather than assumed, which matters more on a release branch than on develop:

* `policy/fees.cpp` gates `fee_estimates.dat` on a hardcoded `42199` literal, not `CLIENT_VERSION`
* wallet readability is gated on `FEATURE_LATEST`, a separate constant family

Confirmed empirically: a wallet created by a patched build reopens on an **unpatched v4.22.9 build** with `walletversion 169900` and its keypool intact.

### Surfaces updated

`configure.ac`, `src/clientversion.h`, `src/clientversion.cpp`, all six Windows `.rc` files, `share/qt/Info.plist.in`, `share/setup.nsi.in`, `build_msvc/bitcoin_config.h`, the cppcheck lint macro list, and `doc/release-process.md`.

Windows `VERSIONINFO` already reserved a fourth field that `rc.exe` was zero-padding, and `setup.nsi.in` carried a literal `.0` in the same slot. `build_msvc/bitcoin_config.h` is hand-maintained because MSVC never runs configure, and was stale at 4.22.6; it is corrected as well as extended.

The `dnl` comment above `CLIENT_VERSION_IS_RELEASE` on this branch asserted that `CLIENT_VERSION` was unchanged at 42209. That stops being true here, so it is reworded rather than left to mislead a future reader.

## 2. Update-check parsing

The `checkupdates` handler matched versions with an **unanchored** `regex_search` for exactly three components. On a four-component string the engine slides the start position until the trailing `$` can match, dropping the major:

| Input | Captured |
|---|---|
| `4.22.9` | `4.22.9` |
| `4.22.9.4` | **`22.9.4`** |
| `v4.22.9.3` | **`22.9.3`** |

The result is silent rather than loud. semver parses `22.9.4` happily and it compares greater than any real release, so neither `remoteV > localV` nor `remoteV == localV` holds: no update offered, no message shown. **A node would never learn a newer release exists.**

This is **pre-existing on this branch and already live**: `v4.22.9.3` is a published tag, arrives as `tag_name` from the GitHub API, and parses as `22.9.3` today. Shipping 4.22.9.4 without this fix would mean 4.22.9.4 nodes never see future updates either.

The expression is anchored, the separators escaped, and the fourth component optional. semver has no fourth field, so it continues to order the first three plus any prerelease tag, and the build number breaks ties. `4.22.9` and `4.22.9.0` stay equal, `4.22.9.4` sorts above both, `4.22.10` above all of them. Three-component behaviour is unchanged, prerelease ordering included.

Note this lands in `src/rpc/server.cpp` here rather than `src/node/update_check.cpp`. The `node::CheckForUpdates` refactor is not on this branch, so the logic still sits inline in the RPC handler. The parsing block itself is identical to the develop-side commit.

## Testing

The identical change is built and fully tested on develop under #487: 488 unit cases, three version-adjacent functional tests, and a live `checkupdates` call. The parsing was checked against a 14-case matrix covering three- and four-component input, prerelease ordering, and malformed strings.

On this branch, expect from a build:

```
"version": 4220904,
"subversion": "/Reddoshi:4.22.9.4/",
```

Two caveats specific to this branch, both pre-existing and neither introduced here:

* the full `util_tests` suite dies with `double free or corruption` on the **unmodified** base, so a failing unit run here proves nothing about this change
* the functional suite does not run at all: `test_framework.py` looks for `src/bitcoind` rather than `src/reddcoind`, and the shared 199-block cache needs staking above `nLastPowHeight = 89`

So the meaningful checks here are the build itself, plus a runtime `getnetworkinfo` and `checkupdates`.

## Compatibility note for reviewers

`getnetworkinfo`'s `version` field changes shape, from `42209` to `4220904`. Anything downstream parsing it as a fixed-width five-digit number needs to accommodate that. The P2P handshake version is `PROTOCOL_VERSION` and is untouched.

One inherited behaviour changes: `qt/optionsmodel.cpp` compares the stored settings version against a literal `130000` from upstream, which was permanently true under the old encoding and is now false. That migration targeted a Bitcoin Core 0.13 dbcache default and was never meaningful here.
