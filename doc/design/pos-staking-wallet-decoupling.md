# PoS Staking / Wallet Decoupling Design Document

**Component:** `src/miner.cpp`, `src/staker.cpp`, `src/pos/`, `src/rpc/mining.cpp`
**Issue:** [RED-46](https://reddink.youtrack.cloud/issue/RED-46) (relates to RED-43)
**Last Updated:** 2026-07-23
**Status:** Implemented on branch `pos/nowallet-staking-guard`. `--disable-wallet`
builds `reddcoind`, `reddcoin-qt` and `test_reddcoin`, and `test_reddcoin` passes
(442 cases). Phase 6 (interface hygiene) is optional and still open.

---

## Executive Summary

The `no-wallet` CI job (`ci/test/00_setup_env_native_nowallet.sh`, `DEP_OPTS="NO_WALLET=1"`)
fails to **link** `reddcoind`. It is the last remaining major job failure in the
RED-43 Cirrus-to-GitHub-Actions migration.

Upstream Bitcoin keeps the node process free of any wallet dependency. ReddCoin
breaks that: PoS staking lives in `libbitcoin_server` and calls `CWallet`
directly. When `--disable-wallet` is configured, `libbitcoin_wallet.a` is never
built, and the link dies on wallet symbols referenced from server objects.

The intent of the config is already recorded in the tree: `src/dummywallet.cpp`
(linked into server when `!ENABLE_WALLET`, `src/Makefile.am:406`) already hides
`-stake`, `-staking`, `-stakenotify` and `-staketimio`. Staking is meant to be a
wallet feature that is simply absent in a no-wallet build. A no-wallet
`reddcoind` still **validates** PoS blocks correctly, since `src/pos/kernel.cpp`
and `CheckBlockSignature` are wallet-free. It just cannot produce them.

This document describes the fix: introduce an abstract staking interface that
`libbitcoin_server` compiles against and only `libbitcoin_wallet` implements,
removing the layering violation rather than papering over it with `#ifdef`.

**Key outcomes:**
- `--disable-wallet` builds and links; `make check` passes.
- `libbitcoin_server` contains no `CWallet` reference.
- The `CInputCoin` leak in `interfaces::Node` is closed.
- No change to staking behavior, consensus rules, or the argument surface.

---

## 1. Coupling surface

Files in `libbitcoin_server` that reference wallet symbols:

| File | Coupling |
|---|---|
| `src/pos/stake.cpp` | `CreateCoinStake`, `GetStakeWeight(CWallet*)`, `GetStakeWeight(std::set<CInputCoin>&)`, `FinalizeCoinStakeReward` |
| `src/pos/signer.cpp` | `SignBlock(CBlock&, const CWallet&)` only. `CheckBlockSignature` is wallet-free and is **consensus** (`src/validation.cpp:3281`) |
| `src/miner.cpp` | `CreateNewBlock(script, CWallet*, bool*)` PoS branch (`:184-221`, `:248-253`), `PoSMiner()` (`:582-747`) |
| `src/staker.cpp` | `CStakeman` uses `GetWallet()` / `GetWallets()` and `CWallet` flags |
| `src/rpc/mining.cpp` | `generateBlocks(..., CWallet*)` (`:154`), `generateblock` PoS path (`:617-692`), `setstaking` (`:365`), `staking` (`:452`), `getstakinginfo`, `GetStakeWeight(pwallet)` (`:795`) |
| `src/test/util/setup_common.cpp` | `SignBlock(*pblock, *pwallet)` (`:451`) in `libtest_util` |
| `src/interfaces/node.h:156` | `Node::getStakeWeight(std::set<CInputCoin>&, ...)` leaks `CInputCoin` (defined in `src/wallet/coinselection.h`) into the node interface |

Wallet-free and staying put: `src/pos/kernel.cpp`, `src/pos/modifiercache.cpp`,
`CheckBlockSignature`, and `AddStakeSetting` / `RemoveStakeSetting` /
`StakeWallet` (`src/miner.cpp:749-788`, which use `interfaces::Chain` only).
`src/pos/kernelrecord.cpp` uses only the abstract `interfaces::Wallet`, so it has
no link dependency.

---

## 2. Hard constraint: static archive link order

`reddcoind_LDADD` is server-then-wallet (`src/Makefile.am:695`):

```
reddcoind_LDADD = $(LIBBITCOIN_SERVER) $(bitcoin_bin_ldadd)   # wallet is first in bin_ldadd
```

but `test_test_reddcoin_LDADD` is wallet-then-server
(`src/Makefile.test.include:189-194`). With static archives, a naive code move
breaks one binary or the other: an archive scanned earlier cannot pick up symbols
from an archive scanned later unless the defining member was already extracted.

**Invariant: `libbitcoin_wallet` must never call `libbitcoin_server`.**

All calls go server to wallet, through an abstract interface declared in server
and implemented in wallet. Both link orders then work with no `--start-group`
hack. This is the reason `PoSMiner` and `CStakeman` stay in `libbitcoin_server`
rather than moving to the wallet library: only the wallet-touching *bodies* move.

**Known exception, found during Phase 2.** `CreateCoinStake` cannot be made
purely wallet-side: its kernel search needs `CheckStakeKernelHash`
(`pos/kernel.cpp`), `g_txindex` (`index/txindex.cpp`), `OpenBlockFile`
(`node/blockstorage.cpp`) and `DeploymentActiveAfter` (`deploymentstatus.cpp`),
all in `libbitcoin_server`, and the seam itself already passes `CChainState&`.
So `libbitcoin_wallet` does reference server symbols on this path, and the
invariant above holds only in the direction that matters for `--disable-wallet`
(server never names a wallet symbol).

Measured after Phase 3, `nm -uA libbitcoin_wallet.a` against the symbols
`libbitcoin_server.a` defines gives exactly 13 references, all of them from
`wallet/staking.o` and none from any other wallet object:

```
cs_main                 g_txindex              g_versionbitscache
GetCoinAge              GetCoinAgeTimes        GetCoinAgeWeight
CheckStakeKernelHash    GetProofOfStakeReward  GetInflationAdjustment
GetMinFee               OpenBlockFile          VersionBitsCache::State
TxIndex::FindTxPosition
```

Both link orders resolve them, because every one of those server members is
already extracted by `init.o` / `validation.o` before either archive order
reaches `wallet/staking.o`; `reddcoind` and `test_reddcoin` both link. So this
is contained rather than fixed. If it ever does fail, the options are
`--start-group` around the two archives, or moving the txindex/blockfile lookup
behind a small server-side helper that the wallet calls through the seam.

---

## 3. Design

### 3.1 The seam

New header `src/interfaces/staking.h`, mirroring the existing
`interfaces::Wallet` / `interfaces::WalletClient` pattern. Method naming follows
that convention's lowerCamelCase, not the `CWallet` PascalCase of the code being
moved.

```cpp
namespace interfaces {

//! Wallet-free description of a stakeable coin, replaces CInputCoin at the boundary.
struct StakeCoin { COutPoint outpoint; CAmount value; };

class StakingWallet {
public:
    virtual ~StakingWallet() {}

    //! RAII handle holding cs_wallet. See section 3.3.
    class Lock { public: virtual ~Lock() {} };
    virtual std::unique_ptr<Lock> lock() = 0;

    virtual std::string getName() const = 0;
    virtual bool isLocked() const = 0;
    virtual bool getEnableStaking() const = 0;
    virtual void notifyStakingStatusChanged() = 0;
    virtual size_t getAvailableCoinCount() = 0;
    virtual bool reserveDestination(CTxDestination& dest, std::string& error) = 0;
    virtual void keepDestination() = 0;
    virtual void abandonOrphanedCoinstakes() = 0;
    virtual void setLastCoinStakeSearchInterval(int64_t interval) = 0;
    virtual bool createCoinStake(CChainState&, unsigned int nBits, int64_t nSearchInterval,
                                 CMutableTransaction& tx_new, const Consensus::Params&) = 0;
    virtual bool finalizeCoinStakeReward(CChainState&, CMutableTransaction& tx_coinstake,
                                         const CAmount& fees, const Consensus::Params&) = 0;
    virtual bool signBlock(CBlock& block) = 0;
    virtual bool getStakeCoins(std::vector<StakeCoin>& coins) = 0;
};

//! Replaces GetWallets() / GetWallet() / GetWalletForJSONRPCRequest.
class StakingSupport {
public:
    virtual ~StakingSupport() {}
    virtual std::vector<std::unique_ptr<StakingWallet>> getStakingWallets() = 0;
    virtual std::unique_ptr<StakingWallet> getStakingWallet(const std::string& name) = 0;
    virtual std::unique_ptr<StakingWallet> getStakingWalletForRequest(const JSONRPCRequest&) = 0;
};

} // namespace interfaces
```

### 3.2 Registration

Follows the existing `NodeContext::wallet_client` pattern
(`src/node/context.h:57`): add

```cpp
interfaces::StakingSupport* staking_support{nullptr};
```

to `NodeContext`, set from `WalletInit::Construct()` (`src/wallet/init.cpp:140`),
which is already `ENABLE_WALLET`-only via `src/Makefile.am:403`. It stays null in
a no-wallet build, and every staking entry point degrades to a clear "no wallet
support compiled in" message.

### 3.3 Lock scope

This is the delicate part of the change.

Server code currently holds `cs_wallet` across whole server-side operations:

| Site | Span |
|---|---|
| `src/miner.cpp:689-690` | all of `CreateNewBlock` |
| `src/miner.cpp:719-720` | `SignBlock` |
| `src/rpc/mining.cpp:634` | `GetReservedDestination` |
| `src/rpc/mining.cpp:642-654` | all of `CreateNewBlock` |
| `src/rpc/mining.cpp:684-687` | `SignBlock` |

`CreateNewBlock` calls back into the wallet several times
(`AbandonOrphanedCoinstakes`, `CreateCoinStake`, `SetLastCoinStakeSearchInterval`,
`FinalizeCoinStakeReward`), so per-method internal locking cannot reproduce that
span. Hence `StakingWallet::lock()` returning an RAII handle: the caller keeps
the exact same span, and the implementation returns an object holding
`LOCK(m_wallet->cs_wallet)`.

**Lock order to preserve:** `cs_wallet` is taken *before* `cs_main` on this path
(`miner.cpp:689`, then `CreateNewBlock`'s `LOCK2(cs_main, m_mempool.cs)` at
`:156`, and `stake.cpp:158` `LOCK2(cs_main, pwallet->cs_wallet)` re-entering the
recursive `cs_wallet`). Do not reorder.

Clang thread-safety analysis stops seeing across the interface boundary, so a
narrowed lock span here fails silently at runtime rather than at compile time.
This section requires human review, not just green tests.

---

## 4. Implementation phases

Single PR from `pos/nowallet-staking-guard` into `develop`, one commit per phase.

### Phase 1 - Add the seam
Add `src/interfaces/staking.h` only. No behavior change, nothing moves. Register
the header in `BITCOIN_CORE_H` (`src/Makefile.am`).

### Phase 2 - Split `pos/stake.cpp` and `pos/signer.cpp`
- New `src/wallet/staking.{h,cpp}` in `libbitcoin_wallet_a_SOURCES`, holding
  `WalletStakingWallet : interfaces::StakingWallet` and
  `WalletStakingSupport : interfaces::StakingSupport`. Bodies move verbatim from
  `CreateCoinStake`, `FinalizeCoinStakeReward`, `GetStakeWeight(CWallet*)` and
  `SignBlock`.
- `src/pos/signer.cpp` **stays in server**; only `SignBlock` is removed from it,
  and the `wallet/wallet.h` include is dropped from `src/pos/signer.h`.
  `CheckBlockSignature` is consensus and must not move.
- `src/pos/stake.cpp` keeps the chain-side weight loop (`g_txindex` /
  `OpenBlockFile` / `GetCoinAgeWeight`, currently `:18-140`) but retyped from
  `std::set<CInputCoin>` to `const std::vector<interfaces::StakeCoin>&`. Drop the
  `wallet/wallet.h` and `wallet/coincontrol.h` includes from `src/pos/stake.h`.
- Wire up the registration from section 3.2 (`NodeContext::staking_support`,
  set in `WalletInit::Construct()`) so later phases have something to resolve
  against.
- The moved `CWallet`-typed free functions keep public declarations in
  `src/wallet/staking.h` for now, and `miner.cpp`, `rpc/mining.cpp` and
  `test/util/setup_common.cpp` include that header, so the wallet-enabled build
  stays green one phase at a time. Phases 3, 5 and 7 remove those call sites and
  the declarations become internal to `wallet/staking.cpp`. Until then the
  no-wallet build still does not link, which is expected.
- `src/node/interfaces.cpp:259` converts its `std::set<CInputCoin>` to
  `std::vector<StakeCoin>` at the call site; the whole method is deleted in
  Phase 6.

### Phase 3 - `miner.cpp`
- `BlockAssembler::CreateNewBlock(const CScript&, interfaces::StakingWallet* = nullptr, bool* = nullptr)`.
- `PoSMiner(interfaces::StakingWallet&, ...)` stays in server. The
  `ReserveDestination` stack object at `:590` becomes
  `StakingWallet::ReserveDestination` / `KeepDestination`, with the
  implementation owning the `ReserveDestination` lifetime.
  `AvailableCoins(...).size()` at `:604` becomes `GetAvailableCoinCount()`.
- Remove all `wallet/*.h` includes from `src/miner.cpp` and the
  `class CWallet;` forward declaration from `src/miner.h:38`. `src/miner.cpp`
  is then free of wallet symbols, which is the point of the phase.
- Call sites that phases 4, 5 and 7 own still have to compile, so they take the
  minimum adaptation now: they build a `StakingWallet` with
  `MakeStakingWallet()` at the call and keep their existing `cs_wallet` spans.
  That covers `staker.cpp` (`ThreadStaker` takes the `shared_ptr<CWallet>` the
  thread already captured), the three PoS `CreateNewBlock` sites in
  `rpc/mining.cpp`, and `TestChain100Setup::CreateAndProcessPoSBlock`, whose
  `CWallet*` parameter becomes `const std::shared_ptr<CWallet>&`.
- The `cs_wallet` spans in `PoSMiner` are the ones this phase actually moves
  onto the seam: three `LOCK(pwallet->cs_wallet)` blocks become
  `staking_wallet.lock()` handles covering exactly the same statements. See
  section 3.3 - this is the part that needs review by eye.

### Phase 4 - `staker.cpp`
`CStakeman` enumerates via `NodeContext::staking_support` instead of
`GetWallets()`; threads hold a `StakingWallet` instead of raw `CWallet*`. When
`staking_support` is null, `InitWallets` / `LaunchStakingThreads` /
`StakeWalletAdd` log that wallet support is not compiled in and no-op. All
`wallet/*.h` and `interfaces/wallet.h` includes are removed from `staker.cpp`;
`staker.h` forward-declares `interfaces::StakingSupport` / `StakingWallet` and
`Options` gains a `staking_support` pointer, set from `NodeContext` in
`init.cpp` step 14.

Two deviations from the sketch above, both forced by existing code:
- Threads hold `std::shared_ptr<StakingWallet>`, not `unique_ptr`.
  `util::TraceThread` takes `std::function<void()>`, whose target must be
  CopyConstructible, so a lambda owning a move-only `unique_ptr` cannot be
  stored. The wallet is still owned for exactly the thread's lifetime.
- `interfaces::StakingWallet` gains `setEnableStaking(bool)` and
  `canStake(std::string& reason)`. `InitWallets` and the launch loop need to set
  the staking flag and skip disable-private-keys / blank wallets, which the
  section 3.1 sketch did not cover. `canStake` returns the reason string so the
  existing per-wallet skip logging is preserved.
- The two `Start()` overloads had duplicated enumeration loops; they now share a
  private `LaunchStakingThreads()`.

### Phase 5 - RPC
`src/rpc/mining.cpp` no longer names `CWallet`: every wallet touch resolves a
`StakingWallet` from `NodeContext::staking_support` through a small
`GetStakingSupport(request)` helper, and all `wallet/*.h` + `pos/stake.h` +
`pos/signer.h` includes are dropped (only `interfaces/staking.h` is added).

- `generatetodescriptor` / `generatetoaddress` resolve
  `getStakingWalletForRequest(request)` (null under `--disable-wallet` or with no
  wallet loaded) and pass the `StakingWallet*` to `generateBlocks`, which throws
  `RPC_METHOD_NOT_FOUND` if it turns out a PoS block is required and none is
  available. Its `ReserveDestination` / `SignBlock` / `MakeStakingWallet` uses
  become `reserveDestination` / `keepDestination` / `signBlock` and a
  `lock()`-wrapped `CreateNewBlock`.
- `generateblock` and `getblocktemplate` do the same; `getblocktemplate` keeps
  its `LEAVE/ENTER_CRITICAL_SECTION(cs_main)` dance and takes the wallet lock
  only around `CreateNewBlock`.
- `getstakinginfo` uses three new `StakingWallet` methods
  (`blockUntilSyncedToCurrentChain`, `getLastCoinStakeSearchInterval`,
  `getStakeWeight`) instead of touching `CWallet` / `GetStakeWeight(CWallet*)`.
- `staking` and `setstaking` enumerate with `getStakingWallets()` and use
  `getName` / `getEnableStaking` / `setEnableStaking` / `canStake`.

Deviation from the original sketch: **`setstaking` stays in `rpc/mining.cpp`**
rather than moving to the wallet RPC table. With `getStakingWalletForRequest`
and `canStake` on the seam it is fully wallet-free where it is, and it still
needs the server-side `CStakeman` (`StakeWalletAdd/Remove`) and `StakeWallet()`
(now called with `*node.chain` instead of `pwallet->chain()`); moving it to the
wallet library would have introduced *new* wallet-to-server references, the
opposite of the goal. Keeping it in server with a null-`staking_support` guard
(returns `NullUniValue`) is simpler and adds no coupling.

Result: on the fresh objects the only server-to-wallet references left are the
three in `wallet/libbitcoin_server_a-init.o` (the `ENABLE_WALLET`-only
`wallet/init.cpp`), which are absent from a `--disable-wallet` build.
`rpc/mining.o` is clean.

### Phase 6 - Interface cleanup
Goal: remove the wallet-internal `CInputCoin` (`wallet/coinselection.h`) from the
node-facing interfaces `interfaces::Node` and `interfaces::Wallet`.

The original sketch here was to delete `Node::getStakeWeight` and move the weight
computation into a new `Wallet::getStakeWeight`. **That does not link**: the
weight loop needs `g_txindex` / `OpenBlockFile` / `GetCoinAgeWeight`
(`libbitcoin_server`), so implementing it in `wallet/interfaces.cpp` pulls
`wallet/staking.o`'s chain dependencies into every consumer of the wallet
interface, including the wallet-only `reddcoin-wallet` tool, which does not link
`libbitcoin_server`. The two-hop split (wallet supplies coins, node computes
weight) exists precisely to keep the wallet interface chain-free.

Implemented instead: retype the boundary from `CInputCoin` to the wallet-free
`interfaces::StakeCoin` and keep the two hops.
- `Wallet::GetStakeWeightSet(std::set<CInputCoin>&)` becomes
  `Wallet::getStakeCoins(std::vector<StakeCoin>&)`; the impl still selects via
  `CWallet::GetStakeWeightSet` and converts `CInputCoin` to `StakeCoin` inside
  `wallet/interfaces.cpp`, so `CInputCoin` never appears in the header.
- `Node::getStakeWeight(std::set<CInputCoin>&, ...)` becomes
  `Node::getStakeWeight(const std::vector<StakeCoin>&, ...)` and forwards to the
  chain-side `GetStakeWeight()` in `pos/stake.cpp` (kept). The
  `wallet/coinselection.h` include drops from `node/interfaces.cpp`.
- `qt/walletmodel.cpp` collects `StakeCoin`s from the wallet and passes them to
  the node; the `class CInputCoin;` forward declarations drop out of both
  interface headers. `qt/rpcconsole.cpp` and `qt/bitcoingui.cpp` are unchanged
  (they call `WalletModel::GetStakeWeight`, already `#ifdef ENABLE_WALLET`).

Verified: `reddcoin-wallet` still links, and weight values are unchanged.

### Phase 7 - Build system and test lib
- `src/Makefile.am`: add `wallet/staking.cpp` to `libbitcoin_wallet_a_SOURCES`
  and `wallet/staking.h` + `interfaces/staking.h` to `BITCOIN_CORE_H`.
- `src/test/util/setup_common.cpp`: the sketch here (just route the `SignBlock`
  call through the seam) was too narrow. `TestChain100Setup` *embeds* a
  `CWallet` (`m_wallet`) and stakes blocks 90-100 with it, so under
  `--disable-wallet` the whole PoS scaffolding has to be `ENABLE_WALLET`-only:
  `m_wallet`, `m_wallet_notifications`, `stakeBlocks()`,
  `CreateAndProcessPoSBlock()` and the wallet includes are guarded, and the
  constructor stops at the 89th PoW block (no PoS tail).
- The tests that drive that scaffolding are guarded to match:
  `txvalidationcache_tests` (whole suite),
  `validation_chainstatemanager_tests::chainstatemanager_activate_snapshot`,
  `interfaces_tests::findCommonAncestor` and `::hasBlocks`, and the
  block-extension loops in `txindex_tests` / `coinstatsindex_tests`.
  `interfaces_tests::findBlock` was made height-agnostic (reads the tip height)
  rather than guarded, so it runs in both configs.

### Phase 8 (folded in) - Qt `--disable-wallet`
Not in the original plan, but the `--disable-wallet` CI job builds `reddcoin-qt`,
which had its own pre-existing guarding gaps (tracked here under RED-46 rather
than split out):
- `BitcoinGUI::openWebReddcoin/openWebReddlove/openWebWiki/openChatroom/openForum`
  are declared, `connect()`-ed and moc'd unconditionally but were *defined*
  inside `#ifdef ENABLE_WALLET`; the definitions moved out of the guard.
- Note (not a code change): switching an already-built tree to `--disable-wallet`
  in place leaves stale `moc_*.cpp` (the moc rule keys on the `.h`, not on
  `bitcoin-config.h`), which shows up as a wall of `BitcoinGUI has no member
  named ...`. Regenerate the moc files (or clean qt) after a config switch.

---

## 5. Verification

1. **Wallet-enabled regression** (the real risk, block assembly is
   consensus-adjacent):
   - Clean `make -j4`, then the PoS functional suite. Run with `--tmpdir` inside
     the project tree; `/tmp` is too small for PoS datadirs.
   - Targeted: `feature_block.py`, `feature_csv_activation.py`, `mining_*`,
     `wallet_*`, plus staking-specific tests.
   - `FinalizeCoinStakeReward`'s fee-inclusive path (commit `b4951959`) must be
     byte-identical. A mainnet reindex is the strongest available check.
2. **No-wallet build**:
   ```
   CONFIG_SITE=$PWD/depends/x86_64-pc-linux-gnu/share/config.site \
     ./configure --disable-wallet --disable-bench --enable-debug && make -j4
   ```
   `reddcoind` links, `make check` passes, and `reddcoind -regtest -staking`
   starts, reports staking unavailable, and still syncs and validates PoS blocks.
3. **Layering invariant**: `nm -u src/libbitcoin_wallet.a | c++filt` shows no
   symbol defined in `src/libbitcoin_server.a`. This is what keeps both link
   orders working.

   The reverse direction (server naming wallet symbols, which is what actually
   blocks `--disable-wallet`) is easiest to scan on the fresh on-disk objects,
   not the archive: `find src -name 'libbitcoin_server_a-*.o'` then `nm -u`.
   Two caveats when reading this:
   - `wallet/init.cpp` is in `libbitcoin_server_a_SOURCES` under `if
     ENABLE_WALLET` (`Makefile.am:406`), so its object
     `wallet/libbitcoin_server_a-init.o` legitimately names wallet symbols
     (`GetWalletStakingSupport`, `MakeWalletClient`, `BerkeleyDatabaseSanityCheck`).
     In a `--disable-wallet` build that TU is replaced by `dummywallet.cpp`, so
     those references disappear. They are not a coupling to remove.
   - The archive holds two members both named `libbitcoin_server_a-init.o` (from
     `src/init.cpp` and `wallet/init.cpp`), so `ar p ... libbitcoin_server_a-init.o`
     and any archive-level attribution are ambiguous. Scan the object files.

   After Phase 4 the real server-to-wallet coupling is confined to
   `rpc/libbitcoin_server_a-mining.o` (11 symbols: `SignBlock`, `GetWallets`,
   `HasWallets`, `GetStakeWeight(CWallet*)`, `MakeStakingWallet`,
   `GetWalletForJSONRPCRequest`, three `ReserveDestination` methods,
   `CWallet::IsWalletFlagSet`, `CWallet::BlockUntilSyncedToCurrentChain`), all of
   which Phase 5 removes. `miner.o` and `staker.o` are clean.
4. **Lint**: `./ci/lint/06_script.sh` (include ordering, header guards for the two
   new headers).

---

## 6. Out of scope

- Moving `src/pos/kernelrecord.*` out of `libbitcoin_server` into
  `libbitcoin_qt`. It is Qt-only dead weight in server but has no link
  dependency, so it does not block this work. Tracked as a follow-up.
- Any change to staking behavior, consensus rules, or the `-stake` / `-staking`
  argument surface. `src/dummywallet.cpp` already hides those arguments in a
  no-wallet build.
- The TSan libc++ `suggest-override` CI failure, tracked separately.
