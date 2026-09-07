4.22.9.4 Release Notes
==================

Reddcoin Core version 4.22.9.4 is now available from:

[https://download.reddcoin.com/bin/reddcoin-core-4.22.9.4/](https://download.reddcoin.com/bin/reddcoin-core-4.22.9.4/)

This is a maintenance release for the 4.22.9 series. It fixes two wallet
defects that could cause a wallet to be created or re-created on an
unintended key derivation path, restores the `gethdwalletinfo` RPC for
BIP32 wallets, re-enables unknown soft fork warnings on mainnet and
testnet, repairs the update check so that four-component releases are
recognised, and brings the regtest network into a state where the standard
functional test suite can exercise soft fork activation and Proof of Stake
block generation.

This is also the first release whose fourth version digit exists in the
binary rather than only in the git tag. See **Version numbering** below,
which includes a compatibility note for anything parsing the `version`
field of `getnetworkinfo`.

Please report bugs using the issue tracker at GitHub:

[Reddcoin Github Issues](https://github.com/reddcoin-project/reddcoin/issues)

To receive security and update notifications, please subscribe to:

[Reddcoin Discord Channel](https://discord.com/channels/314599721039691776/610562116688281611)

How to Upgrade
==============

Upgrading any wallet software carries the standing risk that an out-of-date
backup does not cover everything the wallet holds. **PLEASE CREATE BACKUPS**.

This is the general precaution carried in every release. It is not a
statement about the defects fixed in this one: the wallet fixes described
under **Wallet key derivation fixes** below do not put funds at risk.

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Reddcoin-Qt` (on Mac)
or `reddcoind`/`reddcoin-qt` (on Linux).

Upgrading directly from a version of Reddcoin Core that has reached its EOL is
possible, but it might take some time if the data directory needs to be migrated. Old
wallet versions of Reddcoin Core are generally supported.

This release contains no consensus changes for mainnet or testnet. No
reindex is required and existing wallet files load unchanged.

Compatibility
==============

Reddcoin Core is supported and extensively tested on operating systems
using the Linux kernel, macOS 10.14+, and Windows 7 and newer.  Reddcoin
Core should also work on most other Unix-like systems but is not as
frequently tested on them.  It is not recommended to use Reddcoin Core on
unsupported systems.

From Reddcoin Core 4.22.0 onwards, macOS versions earlier than 10.14 are no longer supported.

Notable changes
===============

Wallet key derivation fixes
---------------------------

Two structures used on the wallet creation and encryption paths had members
that were never initialised, so their values depended on whatever happened
to be on the stack at the call site. Both are fixed in this release.

- `CHDChain::bBip44` was declared but never assigned by either constructor
  or by `SetNull()`. `GenerateNewBip39Seed()` only wrote the flag on the
  BIP44 branch, so importing a mnemonic as a BIP39 wallet left it
  indeterminate. `GenerateNewKey()` branches on `hd_chain.IsBip44()` to
  choose between `m/44'/coin'/0'/change/index` and
  `m/account'/change'/index'`, so a BIP39 import derived BIP44 addresses
  whenever that byte happened to be non-zero. The flag is serialized under
  `VERSION_HD_BIP39`, so the indeterminate value was written to
  `wallet.dat` and the wallet stayed mislabelled across restarts.

  The value is stable for repeated calls along one code path in one binary
  but varies between builds and between the paths that reach
  `GenerateNewBip39Seed()`, which is why this went unnoticed. The flag is
  now zeroed in `SetNull()` and written unconditionally in
  `GenerateNewBip39Seed()`.

- `WalletOptions::walletType`, `::bits` and `::importing` had no
  initialisers. `CWallet::EncryptWallet()` constructed the struct as a bare
  local and passed it to `LegacyScriptPubKeyMan::SetupGeneration()`, whose
  switch on `walletType` falls through to `GenerateNewBip39Seed()` in its
  default case. Encrypting a plain BIP32 HD wallet could therefore
  regenerate it as a BIP39 wallet. The struct now has default member
  initialisers and `EncryptWallet()` sets the three members explicitly.

  Note that regenerating the seed on encryption is not the defect.
  `EncryptWallet()` calls `SetupGeneration()` with `force` for any HD wallet
  that has no mnemonic, which is inherited Bitcoin Core behaviour. Taking the
  BIP39 branch rather than the BIP32 one is what went wrong.

**No funds are at risk from either defect.** In the BIP39 case the mnemonic
is unchanged and only the derivation path differs, so coins under an
unintended path are recoverable by re-importing the same mnemonic with the
other wallet type. Wallets already written with `bBip44` set keep it, since
it is read back out of the serialized `CHDChain`, so existing BIP44 wallets
load and keep deriving on their existing path.

What remains is a recovery hazard rather than a loss of funds: a mnemonic
restored under the wrong wallet type derives a different set of addresses, so
the wallet looks empty while the coins sit untouched on the chain under the
other path. This is what the backup warning under **How to Upgrade** is
about, and it is why encrypting a wallet warrants a fresh `wallet.dat`
backup: seed regeneration on encryption means a backup taken beforehand does
not cover addresses derived afterwards. A `wallet.dat` backup holds the keys
themselves and so is unaffected by which derivation path a wallet uses.

Both defects are present in v4.22.9. Users who created or encrypted a
wallet with an earlier 4.22.9 build should confirm which derivation path
their wallet is using before relying on backups of the mnemonic alone.

In the GUI the wallet status bar icon reports the scheme and exposes nothing
sensitive, which makes it the appropriate thing to point end users at.
`gethdwalletinfo` reports it over RPC, though not by name: a `mnemonic` field
means the wallet has one, `accountextendedprivkey` and `accountextendedpubkey`
mean the BIP44 path, and neither means a plain BIP32 wallet.

**`gethdwalletinfo` output is private key material.** It returns the
mnemonic, the seed and the root and extended private keys in plain text, so
it must never be pasted into an issue, a chat channel, a screenshot or a
support request, and should not be run while screen sharing. Anyone
relaying these instructions to end users should carry that warning with
them.

Version numbering
-----------------

Reddcoin Core now carries a fourth version component. The scheme is
`MAJOR.MINOR.REVISION.BUILD`, matching the layout Bitcoin Core used
historically: what was previously `BUILD` becomes `REVISION` and keeps its
value of 9, and `BUILD` becomes the new trailing component.

Before this change the fourth digit existed only in the git tag name.
`share/genbuild.sh` sets `BUILD_GIT_TAG` for a clean tagged build and
`clientversion.cpp` prefers it over `PACKAGE_VERSION`, so v4.22.9.3 showed
its tag in the startup banner while `getnetworkinfo` still reported `42209`
and the peer subversion string stayed `/Reddoshi:4.22.9/`, indistinguishable
from 4.22.9, 4.22.9.1 and 4.22.9.2. A point release could not identify
itself to the network or to an operator reading RPC output.

`CLIENT_VERSION` is now computed as:

    1000000 * MAJOR + 10000 * MINOR + 100 * REVISION + 1 * BUILD

giving **4220904** for this release, and the peer subversion string becomes
`/Reddoshi:4.22.9.4/`.

**Compatibility note.** The `version` field of `getnetworkinfo` changes
shape, from a five-digit `42209` to a seven-digit `4220904`. Anything
downstream that parses it as fixed-width needs to accommodate that. The
subversion string now always renders four components, so a release with no
build number appears as `4.22.9.0` rather than `4.22.9`. The P2P handshake
version is `PROTOCOL_VERSION` and is unchanged.

The new encoding is deliberately ordered above every value the previous
three-component encoding could produce, which matters because
`CLIENT_VERSION` drives decisions and not only display: `wallet/walletdb.cpp`
rewrites the stored version when the loaded one is older,
`qt/optionsmodel.cpp` migrates QSettings on the same comparison, and
`policy/fees.cpp` compares a required version on load. The old encoding
could not reach 100000 for a single-digit major, while the new one starts at
4000000.

Downgrade compatibility was verified rather than assumed: `policy/fees.cpp`
gates `fee_estimates.dat` on a hardcoded `42199` literal rather than
`CLIENT_VERSION`, and wallet readability is gated on the separate
`FEATURE_LATEST` constant family. A wallet created by a patched build was
confirmed to reopen on an unpatched v4.22.9 build with `walletversion 169900`
and its keypool intact.

New and Updated RPCs
--------------------

- `checkupdates` now recognises four-component versions. The handler matched
  release versions with an unanchored `regex_search` for exactly three
  components, so on a four-component string the engine slid the start
  position along until the trailing `$` could match, dropping the major
  entirely: `4.22.9.4` matched as `22.9.4`, and `v4.22.9.3` as `22.9.3`.

  The failure was silent rather than loud. semver parses `22.9.4` happily,
  and it compares greater than any real Reddcoin release, so neither
  `remoteV > localV` nor `remoteV == localV` held: no update was offered and
  no message was shown. **A node would never learn that a newer release
  exists.**

  This already affects v4.22.9.3 in the field. That tag arrives as
  `tag_name` from the GitHub API and parses as `22.9.3` today, so nodes
  running it are not being told about new releases. Adding a fourth
  component to `PACKAGE_VERSION` would have extended the same fault to the
  local side.

  The expression is now anchored, the separators escaped, and the fourth
  component optional. semver has no fourth field, so it continues to order
  the first three plus any prerelease tag and the build number breaks ties:
  `4.22.9` and `4.22.9.0` stay equal, `4.22.9.4` sorts above both, and
  `4.22.10` above all of them. Three-component behaviour is unchanged,
  prerelease ordering included, so a release candidate still sorts below its
  release.

  Unrecognised input now throws with the offending string rather than being
  searched for something plausible. The return value of `regex_search` was
  previously discarded, so a failed match fell through to `semver::parse("")`
  and surfaced as a bare "Invalid version" with no indication of what had
  been read.

- `gethdwalletinfo` no longer aborts on BIP32 wallets. It declared
  `RPCResult::Type::OBJ` but returned a null value whenever the HD chain
  had no BIP39 seed, and `RPCHelpMan::HandleRequest` asserts that the
  returned value matches the declared type, so the call died with an
  internal bug report from `rpc/util.cpp`. `CHECK_NONFATAL` is ungated, so
  release builds were affected too.

  Every wallet created over RPC hit this: the BIP39 seed field is only ever
  written by `GenerateNewBip39Seed()`, and `createwallet` on this branch
  has no BIP39/BIP44 parameters, that surface being available only through
  the GUI wallet creation wizard.

  A BIP32 wallet is not missing its seed, it stores it differently, as an
  ordinary key in the keystore referenced by `hd_chain.seed_id`. The RPC
  now looks it up that way and reports the seed, root key and derived
  extended keys. Mnemonic fields remain conditional on a mnemonic being
  present and account-level keys remain conditional on the BIP44 path, so a
  BIP32 wallet gets the fields that apply to it with no empty placeholders.
  `accountextendedprivkey` and `accountextendedpubkey`, which were returned
  on the BIP44 path but never documented, are now declared in the RPC help,
  and the conditional fields are marked optional.

  Wallets with no HD seed at all now return `RPC_WALLET_ERROR` rather than
  an internal bug report.

  Note that this RPC dumps private key material, as its help has always
  stated. Now that it returns successfully for BIP32 wallets rather than
  failing, it will be reached far more often, so it is worth restating: the
  response carries the mnemonic, the seed and the root and extended private
  keys in plain text and must be treated exactly like `dumpwallet` output.

- `generatetoaddress` and `getblocktemplate` can now produce Proof of Stake
  blocks. Past `nLastPowHeight` the chain is PoS-only, but both still built
  PoW templates, which on regtest produced blocks rejected with `pow-ended`
  so that `generatetoaddress` returned without generating anything. When
  the next height is past `nLastPowHeight` and a wallet is available, the
  PoS path is taken instead: `CreateNewBlock` sources a coinstake from the
  wallet, `SignBlock` signs the block, and it is submitted via
  `ProcessNewBlock`. PoW heights continue to work on a wallet-less node.

  This change is confined to the RPC layer. No consensus code is affected.

Network warnings
----------------

The unknown-versionbit warning mechanism has been dead code on mainnet
since the Core 22 consensus migration. `MinBIP9WarningHeight` was set to
`INT_MAX` in that migration, which makes the first clause of
`WarningBitsConditionChecker::Condition()` permanently false, so an unknown
soft fork signalled on mainnet produced no `warnings` field in RPC output,
no GUI banner, no `-alertnotify` and no `debug.log` entry.

Mainnet and testnet now use the upstream convention of activation height
plus one confirmation window, derived from actual Reddcoin chain history:

- **Mainnet**: `heightincb` and `cltv` both activated at 5558400, giving
  5558400 + 14400 = **5572800**.
- **Testnet**: all four BIP9 deployments (`heightincb`, `cltv`, `csv`,
  `segwit`) activated at 491904, giving 491904 + 2016 = **493920**. The
  previous value of 483840 was inherited verbatim from Bitcoin along with
  its "segwit activation height" comment, and is meaningless here: block
  483840 on Reddcoin testnet is a legacy `nVersion=5` block several
  thousand blocks before any deployment activated.

Both values were checked against a full scan of the respective chains.
Only bits 0 and 1 have ever been signalled on mainnet, and only bits 0
through 3 on testnet, all of them known deployments. Signalling stops
cleanly at the activation height on both chains, so no historical window
can retroactively drive the warning checker to `LOCKED_IN` or `ACTIVE` and
produce a false "Unknown new rules activated" warning on upgrade, and the
raised gate cannot mask a real warning.

Signet is unchanged. Reddcoin signet has no chain history to derive a value
from.

Test and development changes
----------------------------

The regtest network has been reworked so that soft fork activation and PoS
block generation can be exercised by the functional test suite. These
changes affect regtest only and have no effect on mainnet, testnet or
signet.

- CLTV (BIP65) signals from genesis and activates at height 432 via BIP9,
  using the regtest confirmation window of 144 blocks and a 75% threshold.
- CSV, SegWit and Taproot are set to `NEVER_ACTIVE`, matching mainnet,
  where all three are absent from `getblockchaininfo`. Their bit
  assignments are unchanged, so signalling from an upgraded peer still
  lands on bits this node reports as unknown, which is what drives the
  "Unknown new rules activated" warning in `UpdateTip`. A test that needs
  one of them active passes it explicitly, for example
  `-vbparams=segwit:0:9223372036854775807:0`.
- `MinBIP9WarningHeight` is 0, since regtest chains start at height 0 with
  no pre-existing deployment history.
- The assumeutxo hash and the buried deployment heights for CSV and SegWit
  have been updated for PoS blocks.
- Extended key prefixes now use the testnet values `tpub` (`0x043587CF`)
  and `tprv` (`0x04358394`) instead of the mainnet `xpub`/`xprv` values.
  Regtest was using mainnet prefixes, which broke descriptor tests that
  supply standard `tpub`/`tprv` keys. This matches upstream convention.

Build System
------------

- `depends` no longer byte-compiles `xcb_proto` on install. The `py-compile`
  script bundled with xcb-proto 1.10 imports the `imp` module, which was
  removed in Python 3.12, so staging the package failed outright on newer
  systems including the container images used for CI. The generated `.pyc`
  and `.pyo` files were already deleted again in `postprocess_cmds`, so the
  output is unchanged.

- The Guix time-machine repository URL can be overridden through the
  `GUIX_TIME_MACHINE_URL` environment variable, defaulting to the existing
  Savannah URL. Savannah can serve a malformed redirect that libgit2
  rejects as a cross-host redirect, killing the build before it starts;
  the URL was previously hardcoded and could not be worked around, because
  passing a second `--url` through `ADDITIONAL_GUIX_TIMEMACHINE_FLAGS` has
  no effect.

- The fourth version component is carried through every packaging surface:
  the six Windows `.rc` files, `share/qt/Info.plist.in`,
  `share/setup.nsi.in` and the cppcheck lint macro list. The Windows
  `VERSIONINFO` resource already reserved a fourth field that `rc.exe` was
  zero-padding, and `setup.nsi.in` carried a literal `.0` in the same slot;
  both now receive the real build number.

- `build_msvc/bitcoin_config.h` has been corrected as well as extended. It
  is hand-maintained, because MSVC never runs `configure`, and had gone
  stale at 4.22.6.

- Copyright headers and the copyright year have been refreshed, and the man
  pages and the example `reddcoin.conf` have been regenerated.

Low-level changes
=================

RPC
---

- `gethdwalletinfo` marks its conditional result fields optional and
  declares `accountextendedprivkey` and `accountextendedpubkey`. The
  unreachable `if (!pwallet) return NullUniValue` guard has been removed;
  `GetWalletForJSONRPCRequest()` either returns a wallet or throws.

- `getblocktemplate` resolves the wallet while still holding `cs_main`,
  then releases `cs_main` before taking `cs_wallet`, to preserve lock
  order.

Wallet
------

- `CHDChain::SetBip44()` no longer has a default argument, so a bare call
  can no longer silently mean `true`.

GUI
---

- One inherited behaviour changes as a consequence of the version
  reweighting. `qt/optionsmodel.cpp` compares the stored settings version
  against a literal `130000` carried over from upstream. That comparison was
  permanently true under the old encoding and is now false. It targeted a
  Bitcoin Core 0.13 dbcache default and was never meaningful here.

4.22.9.4 change log
===============

A detailed list of changes in this version follows. To keep the list to a manageable length, small refactors and typo fixes are not included, and similar changes are sometimes condensed into one line.

### Reddcoin commit history 4.22.9.4
 - #080f5bcce consensus: Enable CSV and SegWit deployments on regtest (John Nash)
 - #58db34121 chainparams: Set regtest MinBIP9WarningHeight to 0 (John Nash)
 - #8d1fb526b chainparams: Update regtest assumeutxo and buried deployment heights (John Nash)
 - #62a391c49 fix: Use testnet extended key prefixes for regtest (John Nash)
 - #9351eb357 consensus: Enable CLTV (BIP65) BIP9 signaling in regtest (John Nash)
 - #af5457784 rpc: Enable PoS block generation for generatetoaddress and getblocktemplate (John Nash)
 - #7f837e0e9 depends: Skip byte-compiling xcb_proto on install (John Nash)
 - #ced15b87d consensus: Make CSV, SegWit and Taproot never active on regtest (John Nash)
 - #a650957de build: Mark this branch as a development build, not a release (John Nash)
 - #bf4c801c9 guix: Allow overriding the time-machine repository URL (John Nash)
 - #1deeabbce wallet: initialise bBip44 so bip39 imports stop deriving BIP44 paths (John Nash)
 - #094cba189 wallet: initialise WalletOptions before passing it to SetupGeneration (John Nash)
 - #f5f5fbdcc rpc: return HD info for bip32 wallets instead of throwing (John Nash)
 - #63c2968ee chainparams: enable BIP9 unknown-versionbit warnings on mainnet (John Nash)
 - #c54f25719 chainparams: derive testnet BIP9 warning height from Reddcoin history (John Nash)
 - #635f96ad8 build: add a fourth version component, CLIENT_VERSION_REVISION (John Nash)
 - #12f6438d9 rpc: parse four-component versions in the update check (John Nash)
 - #eae8961b9 scripted-diff: Bump copyright headers (John Nash)
 - #d5eaab860 build: bump copyright year (John Nash)
 - #a5213a647 build: generate man pages (John Nash)
 - #bc46917e6 build: generate default config file (John Nash)

Credits
=======

Thanks to everyone who directly contributed to this release:

- John Nash

As well as to everyone that helped with translations on
[Transifex](https://www.transifex.com/reddcoin/reddcoin/).
