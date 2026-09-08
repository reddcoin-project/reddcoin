4.22.9.5 Release Notes
==================

Reddcoin Core version 4.22.9.5 is now available from:

[https://download.reddcoin.com/bin/reddcoin-core-4.22.9.5/](https://download.reddcoin.com/bin/reddcoin-core-4.22.9.5/)

This is a maintenance release for the 4.22.9 series. It repairs several
Proof of Stake staking defects that cost stakes or left a wallet unable to
stake until the node was restarted, restores the block header's Proof of
Work check, which had been a stub since the PoSV migration, secures the
update check, which previously performed no TLS verification at all, and
adds a verified upgrade path: the client can now name the exact build a
machine needs, download it, and prove it against a signing key compiled
into the binary before offering it to the user.

Unloading a wallet while it was staking was a use-after-free on this
release line, and closing the window that caused it left a second one in
which the same unload hung and never returned. Anyone running a node that
unloads wallets at runtime should upgrade.

Please report bugs using the issue tracker at GitHub:

[Reddcoin Github Issues](https://github.com/reddcoin-project/reddcoin/issues)

To receive security and update notifications, please subscribe to:

[Reddcoin Discord Channel](https://discord.com/channels/314599721039691776/610562116688281611)

How to Upgrade
==============

Upgrading any wallet software carries the standing risk that an out-of-date
backup does not cover everything the wallet holds. **PLEASE CREATE BACKUPS**.

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Reddcoin-Qt` (on Mac)
or `reddcoind`/`reddcoin-qt` (on Linux).

Upgrading directly from a version of Reddcoin Core that has reached its EOL is
possible, but it might take some time if the data directory needs to be migrated. Old
wallet versions of Reddcoin Core are generally supported.

No reindex is required and existing wallet files load unchanged.

This release contains no soft fork or deployment changes for mainnet or
testnet. It does restore one validation check that had been missing: block
headers claiming Proof of Work are now checked against their target again.
This is a tightening rather than a relaxation, it brings the node into line
with the rules the chain was built under, and the existing mainnet and
testnet chains pass it. See **Proof of Work verification restored** below.

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

Proof of Stake staking fixes
----------------------------

Six defects on the staking path are fixed. Between them they cost stakes,
left wallets holding coins they should have been able to stake, and could
crash or hang a node outright.

- **Unloading a staking wallet was a use-after-free.** The staking thread
  was handed a bare `CWallet*`, so nothing held a reference and
  `UnloadWallet()` destroyed the wallet while the thread was still
  dereferencing it on every pass. The Qt wallet-close path reaches the same
  code, so closing a wallet in the GUI had the same effect.

  The thread now captures the wallet as a `shared_ptr` so it cannot be
  destroyed underneath it, and subscribes to the wallet's unload
  notification so it stops and releases its reference rather than turning
  the crash into a hang. Both halves are needed; either alone is not a fix.
  Shutdown was never affected, since `stakeman->Stop()` runs before wallets
  are unloaded. This only ever mattered for a runtime unload.

- **`unloadwallet` could hang for good against a starting staking thread.**
  The fix above closes the use-after-free but left a window at the other
  end of it. `UnloadWallet()` fires the unload notification and then waits
  for the wallet's last reference to be released, while a staking thread
  only subscribes to that notification once it is already running, several
  statements after the reference was taken and the thread started. An
  unload arriving in between notified nobody: the thread went on to wait
  for a notification that had already been sent, holding the very
  reference the unload was blocked on. The call never returned, taking an
  HTTP worker with it until the node was restarted.

  The wallet is now marked before the notification is fired, and the
  thread checks that mark once it has connected, so either the slot runs
  or the flag is already set and the window is covered from both sides.
  The flag lives on the wallet rather than being read from the existing
  name-keyed unloading set, so a wallet reloaded under the same name is
  not mistaken for the one still unloading.

- **`setstaking false` could not be undone.** `setstaking` set the wallet's
  staking flag and nothing else. The staking loop returns on that flag, so
  a `false` ended the thread and a subsequent `true` could not bring it
  back: the only launcher was reachable from node startup. A wallet
  switched off stayed off until the node was restarted or the node-wide
  staking switch was cycled.

  Nothing reported this. `getstakinginfo` showed staking disabled with a
  zero search interval while the `staking` RPC still counted the thread
  that had already returned, so the node looked like it was staking. The
  RPC now notifies the staker directly, adding the thread on a `true` and
  removing it on a `false`.

  A node already running an affected build can recover without a restart by
  cycling the global switch: `staking false` followed by `staking true`.

- **The staking thread lifecycle had a data race and three related
  bookkeeping faults.** The thread's identity was written by the thread
  itself once it started running and read from elsewhere without the lock,
  so a stop arriving before the thread had registered found nothing to stop
  and left the thread behind. Enabling an already-staking wallet launched a
  second thread and overwrote the entry that made the first reachable.
  Stopping joined while holding the lock, so disabling staking could block
  the staking RPC for as long as a staker sleep, which is a minute after a
  block is found. The thread count included threads that had already
  returned.

  There is now one entry per wallet, created by whoever launches the thread
  rather than by the thread itself, owning the interrupt that stops just
  that thread and a flag the thread sets as it returns. Joins happen with
  the lock released. The staker also slept on the connection manager's
  interrupt, which was never signalled for staking, so a locked wallet
  could not return its thread at all; every sleep now waits on the thread's
  own interrupt and every waiting loop checks all three reasons to stop.
  That also removes a null pointer dereference in the sleep taken when no
  connection manager exists.

- **A staked block could be rejected by the node that staked it.** The
  staker sampled the chain tip before calling `CreateNewBlock()` and without
  holding `cs_main`, then used that pointer to rewrite the coinbase with
  `nHeight + 1`. `CreateNewBlock()` re-reads the tip under `cs_main`, so
  when the tip advanced between the two reads the block was built on height
  H while its coinbase claimed H, where H + 1 is required. The node then
  rejected a block it had just staked and signed with `bad-cb-height`, and
  the stake was lost. Observed twice in ten days on a mainnet staker, 2 of
  2311 staked blocks. The coinbase height is now taken from the template's
  own parent.

- **Orphaned coinstakes are abandoned when their block is disconnected**,
  rather than by sweeping the whole wallet before every block template.
  The old sweep ran from `CreateNewBlock` on the staker thread and on every
  `generatetoaddress`, `generateblock` and `getblocktemplate` call, so a
  wallet with a few hundred stakeable outputs paid for a full `mapWallet`
  scan about once a second, each orphan found costing its own database
  write transaction.

  Abandoning is what returns the staked input to the wallet: until the
  orphan is abandoned the coin is held back as spent. Because the old sweep
  only ran from the block template path, a wallet that was not staking never
  abandoned an orphan at all while it was running, and its spendable balance
  stayed short until the next restart. A full sweep still runs once at
  startup, for a wallet that was offline across the reorg.

Verified release upgrades
-------------------------

The client can now identify, download and cryptographically verify the
release build for the machine it is running on. **Nothing is installed and
nothing is executed on the user's behalf**: the sequence ends with a
verified file on disk and the platform's own next step offered to the user.

- **The exact artifact is named.** Every release publishes a separate build
  per host, named after the host it was built for, so a running binary can
  map itself to the file its user needs without any server API, platform
  sniffing or manifest crawling. `configure` now records the host triplet
  as `HOST_TRIPLET`, and both the GUI build and the daemon build are
  resolved from it. Previously the update notice could only link to a
  directory holding fifteen files.

  The triplet cannot be used verbatim: release artifacts are named after
  the three-field alias guix is invoked with, while `HOST_TRIPLET` records
  autoconf's `$host`, which config.sub has expanded to four fields. The two
  agree for Windows and macOS and differ for every Linux host, so the
  vendor field is normalised away. Both forms are accepted, since a build
  from source legitimately produces the canonical one.

  A host nobody publishes a build for resolves to nothing rather than to a
  guess, and falls back to the directory link. So does a prerelease, whose
  artifact naming has never been exercised. On macOS the build is named as
  the Intel one, since only an x86_64 macOS build is published and it runs
  on Apple Silicon under Rosetta.

- **The name is checked before it is shown.** A HEAD probe confirms the
  file exists. The probe distinguishes a definite 404 from a failure to
  find out, and fails open: only the server saying the file is not there
  withdraws the name, because a probe that could not reach the server is no
  evidence about the artifact.

- **The download is verified against a key compiled into the binary.**
  Releases are now accompanied by a BIP340 Schnorr signature over the
  `SHA256SUMS` manifest, made with an air-gapped key whose x-only public
  key is `23e6b696f2b69cd753de0d8fe3875e989085fbb6f2dc09026ba8ba1df33585db`
  and is pinned in this client. A downloaded artifact is either proven to
  hash to the digest that key signed for it, or it is deleted. This is a
  second, independent signature alongside the existing OpenPGP
  `SHA256SUMS.asc`, which remains the signature for manual verification.

  Verification is entirely local: no key server, no network trust, no
  dependency on the transport. Artifacts are staged under one directory per
  release, other releases are pruned after a success rather than before a
  download, and an already-staged file is recognised by hash rather than by
  name, so a file present under the right name with the wrong contents is
  deleted and refetched.

- **The download streams to disk**, decoding a chunked body incrementally,
  reporting progress and honouring a cancel. It writes to a `.part` file
  and renames only once the framing proves the body complete, so an
  interrupted download never leaves a file that looks finished.

- **The GUI offers the sequence from the update dialog** and the daemon
  from the new `downloadupdate` RPC. On completion, Windows offers to run
  the installer, macOS to open the disk image, and Linux to show the file
  in a folder. Linux stops there deliberately: the install shape is not
  knowable from inside the client, so revealing the file is the honest end
  of the sequence rather than guessing at an archive manager.

Update check security and robustness
------------------------------------

The update check, which is the mechanism responsible for telling users that
security fixes exist, is substantially hardened. Several of these were
exploitable or user-visible in the field.

- **TLS certificates are now verified and SNI is sent.** The check fetched
  over HTTPS but authenticated nothing: the SSL context was constructed and
  never configured, so no trust anchors were installed, verification was
  never requested, and the hostname was never checked against the
  certificate. Any party able to intercept the connection could present any
  certificate and substitute the response.

  The most useful thing that buys an attacker is silently suppressing
  upgrade notices, which is the wrong failure mode for exactly this
  mechanism. It also allowed an arbitrary version string to be injected
  into the comparison.

  Trust anchors come from the operating system rather than a bundle shipped
  in this repository, so the trust list follows the platform's own updates
  and revocations: OpenSSL's winstore loader over the system ROOT store on
  Windows, Security.framework on macOS, and `SSL_CERT_FILE`/`SSL_CERT_DIR`
  or the first well-known distribution bundle elsewhere. OpenSSL's default
  verify paths cannot be used on any host, because `depends` bakes the
  builder's absolute `--openssldir` into libcrypto and that directory does
  not exist on the machine running the binary. Failure to obtain anchors is
  fatal to the fetch rather than a downgrade to an unverified one.

- **The check now has a timeout.** The fetch on this line was fully
  synchronous, and boost.asio's synchronous calls accept none, so a server
  that accepted the connection and then stopped responding held the caller
  for as long as the operating system took to abandon the socket. That
  caller was the RPC, and the GUI at startup.

  Rather than write a third implementation, this release takes develop's
  fetcher wholesale. Arriving with it: a deadline measured as time without
  progress, a ceiling on the whole exchange to bound a server that dribbles
  just fast enough to keep resetting the idle timer, redirect following up
  to five hops with a fresh certificate check on each and any redirect
  leaving HTTPS refused, chunked transfer decoding, and rejection of a
  truncated response rather than returning it as complete.

- **The response is bounded while it is read.** The body was previously
  accumulated with no ceiling, so a hostile or broken server could make the
  client allocate until the process died. The ceiling is one megabyte, two
  orders of magnitude above what the real response runs to, and it is
  enforced inside the read loop rather than after it.

- **The update check no longer runs on the GUI thread.** It was performed
  synchronously in two places, at startup during window construction and
  again every time the update dialog was opened, freezing the window for
  the duration. Measured at about 1.2 seconds on a working network; the
  tail is what matters, since a server that accepts a connection and stops
  responding froze the window for as long as the timeouts allow. It now
  runs on a worker thread.

- **A failed update check no longer displays as a successful one.** The
  dialog tested for an `error` field while the RPC publishes `errors`, so
  the message was never picked up. Worse, the branch that ran instead
  compared two empty version strings, found them equal, and took the "you
  are up to date" path, rendering as a bare `Installed version:` with
  nothing after it. A network failure, a TLS failure and a GitHub
  rate-limit response were all indistinguishable from a successful check.

- **The pre-release warning is shown.** It was read out of the result and
  then discarded, so a pre-release build never displayed the caution
  telling the user not to stake or take payments on it.

- **The status-bar update notice names the new version.** It interpolated
  the local version into a string meant to name the new release, so a node
  one version behind was told to update to the version it was already
  running.

Proof of Work verification restored
-----------------------------------

`CheckBlockHeader` was a stub returning `true`, so `CheckProofOfWork` was
called nowhere in validation on this line and a block's claimed `nBits` was
never tested against any actual work. `GetPoWHash`, the scrypt hash, was
defined but never called from anywhere. The check was removed during the
PoSV migration rather than adapted: the upstream line that was deleted was
wrong for Reddcoin, because it hashed `GetHash()` where our proof of work
is scrypt, but deleting it left nothing in its place.

The check is restored in the form develop already carries: hash
`GetPoWHash()`, skip Proof of Stake blocks, and exempt blocks at or before
`CHECK_POW_FROM_NTIME` to match the historical chain.

What this reaches is the historical Proof of Work era, heights up to
`nLastPowHeight` of 260799 on mainnet, where blocks carry version 1 or 2.
`CheckBlockHeader` takes a `CBlockHeader`, so the Proof of Stake predicate
resolves to the header form, `nVersion <= POW_BLOCK_VERSION`, and every
block produced today carries a versionbits `nVersion` of `0x20000000`. The
check is therefore dormant for current blocks and covers exactly the range
that was unprotected. This is the same reach the check has on develop.

No reindex is required. A voluntary reindex now verifies scrypt Proof of
Work for the historical era, which it previously skipped; the mainnet chain
passes, so this changes nothing about which chain a node accepts, only
about what it checks on the way.

Two supporting fixes come with it:

- `CheckProofOfWork` rejected out-of-range targets with `bnTarget <= 0`,
  which for an unsigned type is only ever true for zero. A compact encoding
  carrying the sign bit was decoded to its magnitude and then accepted as an
  ordinary target, and one whose exponent overflows was accepted as whatever
  decoding left behind. Both are now taken from the decoder's own flags.
  While `CheckBlockHeader` was a stub this guarded nothing; it is now load
  bearing.

- Both nonce-grinding loops in the miner tested against `GetHash()`, which
  is SHA256d, rather than the scrypt `GetPoWHash()`, so they satisfied a
  target the consensus code does not check. Harmless only while the header
  check was a stub; now that it is not, a grind against the wrong hash would
  produce blocks the node itself rejects.

Descriptor wallet key derivation
--------------------------------

Descriptor wallets appended a hardcoded `0'` as the BIP44 coin type,
inherited unchanged from Bitcoin Core, while legacy HD wallets already use
Reddcoin's registered SLIP-0044 value of `4'`. A single seed therefore
produced keys in two different places depending on which wallet type
created it, and anyone restoring a descriptor wallet seed into third-party
tooling would look under coin type 4 and see an empty wallet.

Mainnet descriptor wallets now derive at `m/44'/4'/0'`, `m/49'/4'/0'` and
`m/84'/4'/0'`. The test chains are unchanged, since their coin type is
already 1.

**Existing wallets are unaffected and no migration is needed.** A
descriptor string is built once at wallet creation and written to the
wallet database; loading a wallet reads the stored string back, so wallets
created before this change keep deriving where their keys already are. Only
newly created descriptor wallets use `4'`. Encrypting an unencrypted
descriptor wallet already rotates to fresh descriptors under a new seed;
those now use `4'` as well, and the pre-encryption descriptors are retained
as inactive managers so existing coins stay spendable.

New and Updated RPCs
--------------------

- `checkupdates` gains seven result fields, so a node can say which of the
  files in a release directory the machine asking actually needs:

      hosttriplet          the host triplet this build targets
      platform             short platform name used in artifact filenames
      guiartifact          the file a reddcoin-qt user should install
      guiartifactlink      its direct download link
      daemonartifact       the file a reddcoind user should install
      daemonartifactlink   its direct download link
      artifactbytes        size of the GUI artifact, or -1 if not established

  Both surfaces are reported rather than the node guessing which one the
  caller is, because this RPC is reachable from `reddcoin-cli` and from the
  Qt console alike. On Linux the two are equal, since one tarball carries
  both binaries.

  Every field is present and empty rather than absent when unknown, so the
  shape of the result does not depend on whether the check succeeded, which
  matches how the existing fields behave.

- `downloadupdate` is new. It fetches the current release, checks it against
  the signing key built into the binary, and reports where the verified file
  is. It installs nothing: this client never replaces its own files. It
  takes one optional argument, `daemon` (the default) or `gui`, selecting
  which build to fetch; the two are the same file on Linux, while on Windows
  and macOS the GUI build is the installer or disk image and the daemon
  build is the archive.

  Re-running is cheap: an artifact already downloaded and still hashing
  correctly is not fetched again. `verified` in the result is always true,
  because a file that did not verify is deleted rather than reported.

  The version is not a caller-supplied parameter. Taking one would let the
  RPC be pointed at anything on the server, and the answer to "which
  release" belongs in one place.

Release process
---------------

`contrib/release-signing/sign-release-manifest.py` is added: the offline
tool that generates the release key, derives its public key, and produces
the BIP340 Schnorr signature clients verify. The format is specified in
REP-1018 and the purpose value it derives under in REP-0005. The key is
derived from a 24-word BIP39 mnemonic held air-gapped, generated either
from the platform CSPRNG or from externally supplied entropy.

The documented procedure now covers key generation, which was previously
the one step nothing described, and requires restoring the paper backup and
confirming it derives the same key before that key is recorded. A backup
that has never been restored is a hypothesis.

One correction to the existing procedure travels with it. This line's
release process said to build the published OpenPGP manifest with

    cat "$VERSION"/*/all.SHA256SUMS.asc > SHA256SUMS.asc

which signs a file that is not the one served: `all.SHA256SUMS` carries the
codesignature and debug entries and the published `SHA256SUMS` does not.
The difference varies per release, so this can appear to work and then
produce a BAD signature on the next one. The publishing, signing and
verification sections have been brought across whole.

Build System
------------

- **OpenSSL is updated from 1.1.1s to 3.5.7.** 1.1.1 went out of support in
  September 2023 and stopped receiving fixes of any kind, including for
  security. The download had also stopped working: openssl.org now redirects
  to GitHub, and there is no usable mirror since Bitcoin Core dropped
  OpenSSL and stopped carrying the tarball, so the fetch failed against the
  recorded URL.

  3.5.7 is the current long term support release. The 3.x series needs
  `--libdir=lib`, since it otherwise installs into `lib64` on 64-bit targets
  while everything else in `depends` uses `lib`, which is also the only
  library path the build is given and which breaks Boost detection. boost
  1.71, which `depends` pins, predates OpenSSL 3.0 and its SSL wrapper still
  calls functions the 3.x series deprecated, so the include is wrapped in a
  diagnostic guard to keep those warnings out of `-Werror` builds. OpenSSL
  is reached only through `boost::asio::ssl` in the update check, so nothing
  else in the tree is affected by the change of major version.

- `configure` defines `HOST_TRIPLET` from the autoconf host, next to the
  existing `AC_CANONICAL_HOST`. `TARGET_OS` was too coarse: it cannot
  distinguish x86_64 Linux from aarch64 Linux, and the published artifacts
  differ per triplet.

- `CRYPT32.dll` is added to the Windows allowed-libraries list in
  `symbol-check.py`. Linking crypt32 for OpenSSL's winstore certificate
  loader adds it to the imports of `reddcoind.exe` and `reddcoin-qt.exe`.
  It ships with every supported version of Windows, so it carries no new
  runtime requirement. No equivalent change is needed on macOS, where
  Security and CoreFoundation are already on the Mach-O allow list.

GUI
---

- **The wallet staking icon no longer vanishes for the rest of the
  session.** Removing a wallet hid it and nothing ever showed it again, so
  the first wallet unload of a session took the icon away for every wallet
  rather than just the one that went, along with the click target for
  enabling and disabling staking. The refresh path set the tooltip and the
  pixmap but never visibility, so the icon had no way back.

- **The HD and encryption icons come back too.** These are hidden on the
  same path and, unlike the staking icon, are not missing a `show()`: their
  refresh is only reached from signals that a wallet removal does not emit
  for the wallet that remains, and switching wallets emits nothing either.
  In practice they returned only on a lock, unlock or encrypt of the
  remaining wallet, on loading another wallet, or on a restart. The
  encryption icon is the click target for the lock context menu, so the
  quick lock and unlock control went with it. This one is inherited from
  upstream, which hides the same two icons with no refresh.

Test and development changes
----------------------------

This line's unit test suite did not run to completion. Four stacked defects
are fixed, three of them in the code under test rather than in the tests.

- `RegenerateCommitments` erased `tx.vout` at the witness commitment index
  without checking whether a commitment was present. That index is -1 when
  it is not, as on pre-segwit regtest blocks, so the erase ran on an
  out-of-bounds iterator, corrupted the heap and aborted with "double free
  or corruption" when the transaction was destroyed. This crashed the first
  block of the chain fixture and took down the Qt wallet test in
  `make check`. The upstream guard is restored.

- The chain test fixture set mock time to a hardcoded 2020-08-31 while the
  regtest genesis block is dated 2022-01-19. Since a new block's timestamp
  is floored at the previous block's median time past plus one, every block
  the fixture mined was dated roughly 507 days beyond the mocked "now" and
  was rejected as `time-too-new`. Nothing connected, the chain stayed at
  genesis, and the following assertion fired. This was masked until now by
  the out-of-bounds erase above, which aborted the process first. The start
  time is now taken from the genesis block.

- Several test files were still the unadapted upstream versions, asserting
  Bitcoin's numbers against Reddcoin's consensus rules. `pow_tests` expected
  Bitcoin's retarget results from a function that is a stub here, since
  Reddcoin uses Kimoto Gravity Well; `validation_tests` expected Bitcoin's
  50 COIN initial subsidy and halving schedule rather than Reddcoin's
  genesis, premine and bonus schedule. Both take develop's adaptations, so
  the two lines agree on what the rules are.

- `interfaces_tests`, `wallet_tests` and the Qt wallet test hardcoded a
  chain height of 100 and indexed the chain accordingly. The fixture mines
  `nCoinbaseMaturity` blocks, which is 60 on regtest, so the assertions were
  wrong and the indexing walked off the end of the chain, turning the
  failure into a segfault. These now take the height from the chain rather
  than restating it, so they follow the fixture instead of a number kept in
  step by hand.

New coverage:

- Four cases covering the restored Proof of Work check and each condition
  that switches it off. Each asserts its own premise, so none can quietly
  stop testing anything if the hash or the target changes.
- Table-driven coverage of the host-triplet-to-artifact mapping over every
  host in the guix `HOSTS` list, with each expected filename taken from the
  published 4.22.9.4 `SHA256SUMS`.
- Release manifest verification, including a case that embeds the real
  published 4.22.9.4 manifest and signature and checks they verify against
  the pinned key.
- Update check parsing, download and staging.
- Descriptor wallet key origin paths for each output type on mainnet,
  testnet and regtest.
- The ordering the `unloadwallet` startup-race fix relies on, asserted by
  reading the unloading flag from inside the notification itself.

One unit test was performing real DNS and TLS on every run, while carrying a
comment claiming no network was reached. It is removed; what it asserted is
already covered by a case that is rejected before anything is fetched. Test
suites were verified inside a network namespace with no connectivity rather
than by reasoning about them.

A new debug-only option, `-stakerstartdelay=<n>`, pauses each staking
thread for `<n>` milliseconds before it subscribes to its wallet's unload
notification. That widens the startup window so an unload can be landed
inside it deliberately, which is how the `unloadwallet` race above was
reproduced and confirmed by hand. It defaults to 0 and is hidden from
`-help` unless `-help-debug` is passed.

Low-level changes
=================

RPC
---

- The update check moves out of `rpc/server.cpp` into
  `src/node/update_check.cpp`, matching develop byte for byte apart from
  two forced differences. `checkforupdatesinfo` stays behind as a
  forwarder, so the Qt call sites are untouched. `rpc/server.cpp` no longer
  pulls in boost.asio, OpenSSL, the CA store or the artifact mapping.

Wallet
------

- `AbandonTransaction` gains an overload taking a `WalletBatch`, so a sweep
  costs one database transaction rather than one per transaction abandoned.
  `ReacceptWalletTransactions` uses it.

- A staking wallet re-stakes an input whenever its previous coinstake was
  orphaned, and reported that with the inherited upstream conflict wording,
  which on Reddcoin reads like a double spend for what is the expected
  outcome of normal staking. One mainnet node logged 496 of these in ten
  days with nothing wrong. The inherited line is left untouched so it stays
  diffable against Core, and is followed by a second line for the case that
  is provably benign: a confirmed coinstake displacing a wallet coinstake
  over the same input, which only this wallet could have signed. A coinstake
  conflicting with an ordinary spend stays a real conflict and keeps the
  bare message.

GUI
---

- The update dialog takes an `auto_check` parameter, defaulted on, so it can
  be constructed in tests without performing network requests. Without it
  the dialog tests took eleven seconds of real network traffic; they now run
  in 82 milliseconds.

- The download runs on a worker thread, since staging blocks for as long as
  a 29 MB transfer takes. Progress is thinned to whole percentage points,
  because the underlying callback fires around 180 times a second, and
  cancelling sets a flag the worker polls between reads rather than tearing
  down an in-flight operation.

4.22.9.5 change log
===============

A detailed list of changes in this version follows. To keep the list to a manageable length, small refactors and typo fixes are not included, and similar changes are sometimes condensed into one line.

### Reddcoin commit history 4.22.9.5
 - #156a734f4 wallet: abandon orphaned coinstakes when their block is disconnected (John Nash)
 - #ed39c298e pos: fix the staking thread lifecycle (John Nash)
 - #30eb2d50c pos: restart a wallet's staking thread from setstaking (John Nash)
 - #63ce42800 pos: stop a wallet's staking thread when the wallet is unloaded (John Nash)
 - #8959c65dc gui: keep the wallet staking icon after a wallet is unloaded (John Nash)
 - #1aba33cc1 gui: restore the HD and encryption icons after a wallet is unloaded (John Nash)
 - #ada47c598 gui: name the available version in the update notice (John Nash)
 - #70430cc73 depends: update openssl to 3.5.7 (John Nash)
 - #b9c26e437 rpc: verify TLS certificates and send SNI in the update check (John Nash)
 - #7f6429252 contrib: allow CRYPT32.dll in the Windows symbol check (John Nash)
 - #a11851939 wallet: name the routine re-stake behind a coinstake conflict (John Nash)
 - #63a3607dc miner: use the template's parent when rewriting the coinbase height (John Nash)
 - #645e76f66 build: export the host triplet to C++ as HOST_TRIPLET (John Nash)
 - #9bd23eec5 node: map the host triplet to its published release artifacts (John Nash)
 - #361c6e74a rpc: name the exact artifact this host needs in checkupdates (John Nash)
 - #47cef7a1c qt: name the build this machine needs in the update dialog (John Nash)
 - #6e8c1cc2b qt: read the update check's error field under the name it is published (John Nash)
 - #33e037a3b qt: show the pre-release warning in the update dialog (John Nash)
 - #81ae51ba3 rpc: bound the update check's response (John Nash)
 - #792d20b2b contrib: sign the release manifest for the client (John Nash)
 - #fa32e3a6b contrib: add the missing key generation step (John Nash)
 - #06c8580d3 doc: record the production release key (John Nash)
 - #2b36f3b49 node: give the update check a timeout, by aligning it with develop (John Nash)
 - #c1363ecda node: stream a download to disk, with progress and cancel (John Nash)
 - #13682fecb node: check the artifact is there before naming it (John Nash)
 - #97fe2d997 node: verify a release manifest against the pinned key (John Nash)
 - #764d36529 node: download, verify and stage a release artifact (John Nash)
 - #9b53229c4 rpc: add downloadupdate, which fetches and verifies a release (John Nash)
 - #bb1137917 qt: offer to download and verify the update from the dialog (John Nash)
 - #72fc03bc0 qt: run the update check off the GUI thread (John Nash)
 - #b851d9524 qt: hand the verified build to the platform (John Nash)
 - #4e1ff784d qt: include <memory> where the test uses unique_ptr (John Nash)
 - #259470128 test: stop a unit test performing real network requests (John Nash)
 - #9c9fb00d9 miner: guard witness commitment erase in RegenerateCommitments (John Nash)
 - #5c9e1763c qt: adapt the wallet test to this line's chain fixture (John Nash)
 - #5febdbe13 test: start the chain fixture's clock after genesis (John Nash)
 - #9511aa084 validation: verify proof of work in CheckBlockHeader (John Nash)
 - #f2a94f71c mining: grind the scrypt hash rather than SHA256d (John Nash)
 - #5f8aedb08 test: cover the proof of work check in CheckBlockHeader (John Nash)
 - #62f6a9336 pow: reject negative and overflowing compact targets (John Nash)
 - #04e692553 test: adapt upstream difficulty and subsidy expectations (John Nash)
 - #c8ea83226 test: follow the chain height rather than restating it (John Nash)
 - #d103d3f54 wallet: derive descriptor wallets at Reddcoin's coin type (John Nash)
 - #786f36227 staker: stop unloadwallet hanging on a starting staking thread (John Nash)

Credits
=======

Thanks to everyone who directly contributed to this release:

- John Nash

As well as to everyone that helped with translations on
[Transifex](https://www.transifex.com/reddcoin/reddcoin/).
