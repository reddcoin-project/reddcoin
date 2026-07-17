# BIP39 Unicode (NFKD) Normalization Design Document

**Component:** `src/util/bip39` (CMnemonic)
**Last Updated:** 2026-07-17
**Status:** Proposed — supersedes the ideographic-space-only fix on branch `bip39/japanese-ideographic-space`

---

## Executive Summary

Reddcoin's BIP39 implementation (`CMnemonic`) does not apply the Unicode NFKD
normalization that BIP39 mandates for the mnemonic sentence and passphrase.
Because every non-ASCII wordlist shipped in the tree is stored in **NFKD** form
and word matching is a raw byte comparison, a **valid** French, Spanish,
Japanese, or Korean mnemonic entered in the more common **NFC** form (what
macOS, iOS, and most IMEs emit) fails validation today, and mnemonics/passphrases
that are not already normalized derive a non-interoperable seed.

The recently merged ideographic-space fix (U+3000 -> U+0020) addresses one narrow
instance of this defect for Japanese. This document proposes the general,
spec-compliant fix: vendor `utf8proc` as a `src/` subtree, normalize the mnemonic
and passphrase with NFKD at every parse and derivation point, switch Japanese
mnemonic generation to the canonical ideographic-space separator, and add a
dual-derivation fallback so any wallet created under the old, non-normalized
rules remains recoverable.

**Key outcomes:**
- Valid fr/es/ja/ko mnemonics validate regardless of input normalization form (implemented: none yet; proposed).
- Seeds match upstream BIP39 wallets for non-ASCII mnemonics and passphrases.
- No change to ASCII-only wallets (NFKD is a no-op on ASCII).
- Existing non-ASCII wallets protected by dual-derivation fallback.

---

## Table of Contents

1. Problem Statement
2. Goals and Non-Goals
3. Background: BIP39 Normalization Requirements
4. Design Overview
5. Dependency: Vendoring utf8proc
6. Build Integration
7. Detailed Changes
8. Dual-Derivation Fallback
9. Backward Compatibility and Migration
10. Test Plan
11. Security Considerations
12. Rollout and Effort
13. Alternatives Considered
14. Open Questions

---

## 1. Problem Statement

`CMnemonic::Check` validates a mnemonic by splitting on the ASCII space byte and
comparing each word to the language wordlist with a byte-exact comparison
(`ssCurrentWord == wordlist[nWordIndex]`). `CMnemonic::ToSeed` feeds the raw
mnemonic and passphrase bytes directly into PBKDF2-HMAC-SHA512.

An audit of the eight shipped wordlists shows every one is stored in NFKD
(fully decomposed) form:

| Wordlist            | Non-ASCII words | Stored form | Differs from NFC | Breaks on NFC input today |
|---------------------|-----------------|-------------|------------------|---------------------------|
| english             | 0               | ASCII       | no               | no                        |
| italian             | 0               | ASCII       | no               | no                        |
| french              | 366             | NFKD        | yes (366)        | **yes**                   |
| spanish             | 334             | NFKD        | yes (334)        | **yes**                   |
| japanese            | 2048            | NFKD        | yes (644)        | **yes**                   |
| korean              | 2048            | NFKD        | yes (2048)       | **yes**                   |
| chinese (s/t)       | 2048            | NFKD        | no (CJK stable)  | no                        |

Examples of the stored (decomposed) forms:
- French `académie` = `a c a d e U+0301 m i e` (combining acute accent).
- Spanish `ábaco` = `a U+0301 b a c o`.
- Korean `가격` = `U+1100 U+1161 U+1100 U+1167 U+11A8` (decomposed jamo).
- Japanese words separated by `U+3000` IDEOGRAPHIC SPACE.

Consequences of the missing normalization:
1. **Validation false-negatives.** A user pasting a valid French/Spanish/Japanese/
   Korean mnemonic in NFC form (the default on Apple platforms and most input
   methods) is told the phrase is invalid, because the composed bytes never match
   the decomposed wordlist entries.
2. **Non-interoperable seeds.** Even when a phrase validates, `ToSeed` derives a
   different seed than a compliant wallet whenever the mnemonic or passphrase is
   not already NFKD-normalized. The passphrase is the sharpest edge: it is
   arbitrary user text (for example `㍍` U+338D decomposes under NFKD to
   `メートル`), and today it is hashed verbatim.

The ideographic-space fix currently on `bip39/japanese-ideographic-space` handles
only the `U+3000 -> U+0020` separator case. It is correct but partial: it is a
byte-level special case of the general NFKD requirement and does nothing for NFC
accents, decomposed jamo, or passphrase normalization.

---

## 2. Goals and Non-Goals

### Goals
- Apply full Unicode NFKD normalization to the mnemonic (validation and seed) and
  to the passphrase (seed), matching BIP39 and the trezor/python-mnemonic
  reference implementation.
- Fix the fix generally: French, Spanish, Japanese, and Korean, not Japanese only.
- Keep all secret material (mnemonic, passphrase, intermediate buffers) inside
  secure-allocator memory.
- Emit the canonical `U+3000` separator when generating Japanese mnemonics.
- Preserve recoverability of any wallet created under the previous
  non-normalized behavior via a dual-derivation fallback.

### Non-Goals
- Adding new BIP39 languages or changing wordlists.
- Changing key derivation for ASCII-only wallets (NFKD is a no-op there and no
  behavior changes).
- Reworking the wallet UI beyond what the fallback requires.
- General-purpose Unicode support elsewhere in the codebase (utf8proc is scoped
  to BIP39 mnemonic handling).

---

## 3. Background: BIP39 Normalization Requirements

BIP39's "From mnemonic to seed" step requires that both the mnemonic sentence and
the passphrase be processed with Unicode NFKD normalization before PBKDF2. There
is no space-specific rule; separator handling is a side effect of NFKD:

- `NFKD(U+3000 IDEOGRAPHIC SPACE) = U+0020 SPACE`
- `NFKD(U+00A0 NO-BREAK SPACE) = U+0020 SPACE`

The reference implementation (`trezor/python-mnemonic`) runs
`unicodedata.normalize("NFKD", text)` on the input, then splits words on the
ASCII space and runs PBKDF2. Because the wordlists are themselves stored NFKD,
normalized input words compare equal to the stored entries. Our wordlists already
match this convention (see the audit in Section 1), so once input is
NFKD-normalized, the existing "split on ASCII space" logic continues to work
unchanged.

---

## 4. Design Overview

Introduce a single normalization primitive and apply it at every entry point that
parses or derives from a mnemonic:

```
SecureString NormalizeNFKD(const SecureString& in);   // NFKD, secure-memory safe
```

Application points:

| Function                        | Normalize mnemonic | Normalize passphrase |
|---------------------------------|--------------------|----------------------|
| `Check`                         | yes                | n/a                  |
| `DetectLanguageSeed`            | yes                | n/a                  |
| `getWordCount`                  | yes                | n/a                  |
| `getStrength`                   | yes                | n/a                  |
| `ToSeed`                        | yes                | yes                  |
| `FromData` (generation)         | output already NFKD; emit `U+3000` for Japanese | n/a |

`NormalizeNFKD` subsumes the merged `NormalizeMnemonicSpaces` helper, which is
removed. Word splitting stays "split on ASCII space" because NFKD maps the
ideographic and no-break spaces to ASCII space.

NFKD is implemented with the vendored `utf8proc` library (Section 5), using the
buffer-owning `utf8proc_decompose` / `utf8proc_reencode` API so that no secret
bytes are copied into the general malloc heap (Section 11).

---

## 5. Dependency: Vendoring utf8proc

### Choice

`utf8proc` (JuliaStrings/utf8proc) is the standard lightweight Unicode
normalization library: a single C source plus a generated data table, permissive
license, no transitive dependencies. It is preferred over ICU (heavyweight, and
deliberately avoided upstream in Bitcoin Core) and over a hand-rolled table
(cannot correctly normalize arbitrary passphrases; unsafe to ship a partial
Unicode table in seed-derivation code).

### Layout

Vendored as a `src/` subtree, mirroring `src/secp256k1`, `src/leveldb`, and
`src/crc32c`:

```
src/utf8proc/
  utf8proc.h          # public API
  utf8proc.c          # implementation
  utf8proc_data.c     # generated Unicode decomposition tables
  LICENSE.md          # utf8proc MIT-style license + Unicode data license
  README.md           # upstream readme (provenance)
```

- **Pinned version:** utf8proc v2.9.0 (record the exact upstream commit and
  tarball SHA256 in the subtree commit message for provenance).
- **Update procedure:** documented as a subtree pull, same as other `src/`
  subtrees; no local modifications to utf8proc sources.

### API used for NFKD (secure-memory safe)

```c
// options for NFKD:
const utf8proc_option_t OPT =
    (utf8proc_option_t)(UTF8PROC_STABLE | UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT);

// size pass -> caller-owned secure buffer -> fill -> reencode in place
utf8proc_ssize_t n = utf8proc_decompose(in, len, nullptr, 0, OPT);
SecureVector<utf8proc_int32_t> buf(n);                      // secure_allocator
utf8proc_decompose(in, len, buf.data(), n, OPT);
utf8proc_ssize_t m = utf8proc_reencode(buf.data(), n, OPT); // int32 -> UTF-8, in place
SecureString out(reinterpret_cast<char*>(buf.data()), m);
```

The convenience `utf8proc_NFKD()` / `utf8proc_map()` helpers are **not** used:
they `malloc` the result into the general heap, which would leak secret bytes
into unzeroed, swappable memory.

---

## 6. Build Integration

Add `src/Makefile.utf8proc.include`, mirroring `src/Makefile.crc32c.include`, and
include it from `src/Makefile.am` next to the existing subtree includes
(`src/Makefile.am:899-900`).

```make
LIBUTF8PROC_INT = utf8proc/libutf8proc.a
EXTRA_LIBRARIES += $(LIBUTF8PROC_INT)
LIBUTF8PROC = $(LIBUTF8PROC_INT)

UTF8PROC_CPPFLAGS_INT =
UTF8PROC_CPPFLAGS_INT += -I$(srcdir)/utf8proc
UTF8PROC_CPPFLAGS_INT += -DUTF8PROC_STATIC   # no dllexport/dllimport on Windows

utf8proc_libutf8proc_a_CPPFLAGS = $(AM_CPPFLAGS) $(UTF8PROC_CPPFLAGS_INT) $(UTF8PROC_CPPFLAGS)
utf8proc_libutf8proc_a_CFLAGS   = $(AM_CFLAGS) $(PIE_FLAGS)
utf8proc_libutf8proc_a_SOURCES  = \
    utf8proc/utf8proc.h \
    utf8proc/utf8proc.c \
    utf8proc/utf8proc_data.c
```

Wiring:
- `include Makefile.utf8proc.include` in `src/Makefile.am`.
- `util/bip39.cpp` is compiled into **`libbitcoin_util`**
  (`libbitcoin_util_a_SOURCES`, `src/Makefile.am:621`; block begins at line 602).
  This is the library that carries `CMnemonic`, including the wallet-creation
  `ToSeed` path invoked from `CHDChain::SetMnemonic`. Add `-I$(srcdir)/utf8proc`
  and `-DUTF8PROC_STATIC` to `libbitcoin_util_a_CPPFLAGS`.
- Add `$(LIBUTF8PROC)` to the link lines of every binary that links
  `libbitcoin_util` and references `CMnemonic`: the daemon, the wallet tool, the
  GUI, and `test/test_reddcoin` (`src/Makefile.test.include`). With a static
  convenience lib only the referenced objects are pulled, so binaries that never
  touch `CMnemonic` incur no cost, but listing `$(LIBUTF8PROC)` after
  `$(LIBBITCOIN_UTIL)` on each `LDADD` is the safe, convention-following wiring.
- Note: utf8proc is C. The convenience lib is compiled with `$(CC)`; the header
  is C-linkage-clean and safe to include from C++.

No `configure.ac` feature flags are required (unlike crc32c's SSE/NEON probes);
utf8proc has no platform conditionals beyond `UTF8PROC_STATIC`.

---

## 7. Detailed Changes

### 7.1 `src/util/bip39.cpp`

- **Remove** `NormalizeMnemonicSpaces` (anonymous namespace).
- **Add** `NormalizeNFKD(const SecureString&) -> SecureString` (anonymous
  namespace) implemented as in Section 5. On a utf8proc error return
  (negative length) fall back to returning the input unchanged, so malformed
  UTF-8 degrades to today's behavior rather than throwing.
- **`Check`, `DetectLanguageSeed`, `getWordCount`, `getStrength`:** replace the
  `NormalizeMnemonicSpaces` call (or add one) with
  `mnemonic = NormalizeNFKD(mnemonic);` at the top, before any splitting.
- **`ToSeed`:** normalize both inputs:
  ```cpp
  mnemonic   = NormalizeNFKD(mnemonic);
  passphrase = NormalizeNFKD(passphrase);
  ```
  (canonical derivation; see Section 8 for the legacy path).
- **`FromData` (generation):** join Japanese words with `U+3000` instead of
  ASCII space. All other languages keep ASCII space. Output remains NFKD because
  the wordlists are NFKD. This is cosmetic for interop (import re-normalizes) but
  makes generated Japanese mnemonics spec-canonical.

### 7.2 `src/util/bip39.h`

- Add a derivation-mode parameter to `ToSeed` for the fallback (Section 8), e.g.:
  ```cpp
  enum class MnemonicNorm { NFKD, Legacy };
  static void ToSeed(SecureString mnemonic, SecureString passphrase,
                     SecureVector& seedRet, MnemonicNorm norm = MnemonicNorm::NFKD);
  ```
  `Legacy` skips normalization and reproduces the pre-change byte-for-byte
  behavior.

### 7.3 Wallet restore path

`CHDChain::SetMnemonic` (`src/wallet/walletdb.cpp:68`) calls `ToSeed`. This is the
single seed-derivation site and the anchor for the fallback (Section 8).

---

## 8. Dual-Derivation Fallback

### Rationale

Moving to NFKD changes the derived seed for any wallet whose mnemonic or
passphrase contains non-ASCII, non-normalized bytes. To avoid stranding funds in
such a wallet, restore computes both derivations and adopts the one that actually
holds history.

### Behavior

- **New wallet creation:** always canonical NFKD. No legacy seeds are ever
  created going forward.
- **Restore from mnemonic:**
  1. Compute `seed_nfkd = ToSeed(m, p, NFKD)`.
  2. Compute `seed_legacy = ToSeed(m, p, Legacy)`.
  3. If `seed_nfkd == seed_legacy` (the ASCII-only common case), proceed with the
     single seed; no fallback needed.
  4. Otherwise derive addresses from `seed_nfkd` first. During the initial
     rescan, if `seed_nfkd` shows no history but `seed_legacy` does, adopt
     `seed_legacy` for that wallet, record the choice in the HD chain metadata so
     subsequent loads are deterministic, and **surface a one-time user
     notification** (GUI notification / RPC warning field / debug-log entry)
     stating that the wallet was restored using the legacy (pre-normalization)
     derivation. This makes the non-canonical derivation visible rather than
     silent, so the user can migrate funds to a freshly created NFKD wallet if
     desired.
  5. Default when neither shows history (a genuinely new import): `seed_nfkd`
     (canonical).

### Persistence

Record the chosen normalization mode in the HD chain record so the wallet does
not re-evaluate the fallback on every load and cannot silently switch derivations
after funds arrive. This is an additive field; older wallets without it default
to canonical NFKD, which is correct for the ASCII-only majority.

### Scope note

The fallback only diverges for non-ASCII input. Because the pre-change `Check`
required input to already match the NFKD wordlist, the realistic legacy
population is narrow (chiefly wallets with a non-ASCII passphrase, or Japanese
wallets created via the interim ideographic-space fix). The fallback is cheap
insurance rather than a mass-migration mechanism.

---

## 9. Backward Compatibility and Migration

- **ASCII mnemonic + ASCII passphrase:** `NFKD(x) == x`. Seeds are byte-for-byte
  identical. English and Italian wallets, and every all-ASCII wallet in any
  language, are unaffected. This is the overwhelming majority of wallets.
- **Non-ASCII wallets created before this change:** protected by the
  dual-derivation fallback (Section 8); recoverable without user action beyond a
  normal rescan.
- **Interim ideographic-space branch:** its Japanese seed behavior is a strict
  subset of NFKD, so wallets validated under it derive the same canonical seed
  here. The `japanese_ideographic` test vectors and the space-normalization test
  are retained (Section 10).
- **Chinese:** CJK ideographs in the Chinese wordlists are NFKD-stable, so
  Chinese wallets are unaffected in practice.

---

## 10. Test Plan

Extend `src/test/bip39_tests.cpp` and `src/test/data/bip39_vectors.json`:

1. **NFC-input regression (the headline fix).** For French, Spanish, and Korean:
   take a valid vector mnemonic, re-encode it to NFC, and assert `Check` passes
   and `ToSeed` equals the canonical vector seed. These cases fail on current
   `master` and demonstrate the real-world defect.
2. **NFKD passphrase vectors.** Import the canonical BIP39 Japanese vectors that
   use the passphrase `㍍ガバヴァぱばぐゞちぬfぶぶちぬすぷぬゞ` (the historical
   trezor Japanese test set; the current `vectors.json` only uses passphrase
   `TREZOR`). These exercise NFKD of the passphrase specifically.
3. **Ideographic-space subset.** Keep the existing
   `bip39_japanese_ideographic_space` test (U+3000 / mixed separators) as a
   regression guard; it now passes as a consequence of NFKD.
4. **Idempotence / ASCII no-op.** Assert `NormalizeNFKD` is a no-op on ASCII and
   is idempotent (`NFKD(NFKD(x)) == NFKD(x)`), and that all English/Italian seeds
   are unchanged.
5. **Dual-derivation unit test.** Construct a mnemonic+passphrase whose NFKD and
   legacy seeds differ; assert `ToSeed(..., Legacy)` reproduces the pre-change
   seed and `ToSeed(..., NFKD)` reproduces the upstream seed.
6. **Malformed UTF-8.** Assert `NormalizeNFKD` degrades gracefully (returns input)
   and does not crash.

Vector sourcing (the passphrase-bearing Japanese set) is an explicit task item.

---

## 11. Security Considerations

- **Secret handling.** Mnemonic and passphrase are `SecureString`
  (`secure_allocator`, zeroized on free). Normalization uses `utf8proc_decompose`
  / `utf8proc_reencode` into a `SecureVector<utf8proc_int32_t>` so intermediate
  and output bytes never touch the general malloc heap. The convenience
  allocating helpers are prohibited (Section 5).
- **No new network or file surface.** utf8proc is pure computation over an
  in-memory buffer and an embedded static table.
- **Data provenance.** The Unicode decomposition table (`utf8proc_data.c`) is
  generated upstream from the Unicode Character Database; pin the version and
  record the source hash. No local edits to utf8proc sources, so future subtree
  updates are auditable.
- **Determinism.** NFKD for a fixed Unicode version is deterministic; the seed for
  a given (mnemonic, passphrase, mode) is stable across platforms.
- **Consensus.** None. This is wallet-local key derivation; it does not touch
  block or transaction validation.

---

## 12. Rollout and Effort

Suggested phasing (each phase independently reviewable):

1. **Subtree + build** — vendor `src/utf8proc/`, add
   `Makefile.utf8proc.include`, wire link lines; confirm a clean build links.
2. **Normalization core** — `NormalizeNFKD`, remove `NormalizeMnemonicSpaces`,
   wire into the five parse/derive functions; `U+3000` generation.
3. **Fallback** — `MnemonicNorm` mode on `ToSeed`, restore-path dual derivation,
   HD-chain metadata field.
4. **Tests + vectors** — Section 10.

Rough estimate: ~3 days including review (roughly 1 day subtree/build, 0.5 day
normalization core, 1 day fallback + wallet metadata, 0.5 day tests/vectors).

---

## 13. Alternatives Considered

- **ICU.** Full-featured but heavyweight; a large new dependency surface that
  Bitcoin Core intentionally avoids. Rejected.
- **Hand-rolled NFKD subset.** Table covering only the codepoints in the
  wordlists. Rejected: cannot correctly normalize arbitrary passphrases, and
  shipping a partial Unicode table in seed-derivation code is fragile and
  security-sensitive.
- **Space-normalization only (the interim branch).** Correct but partial; does
  not fix NFC accents, decomposed jamo, or passphrase normalization. Retained as
  a subset and superseded by this design.
- **No fallback (document-only migration).** Simpler, but risks stranding funds
  for the narrow non-ASCII-passphrase population. Rejected in favor of the
  dual-derivation fallback.

---

## 14. Resolved Decisions and Open Questions

Resolved:

1. **utf8proc pin — RESOLVED.** Pin to utf8proc **v2.9.0**. Record the tarball
   SHA256 in the subtree import commit for provenance.
2. **Library placement — RESOLVED.** `util/bip39.cpp` compiles into
   **`libbitcoin_util`** (`libbitcoin_util_a_SOURCES`, `src/Makefile.am:621`).
   Include flags on `libbitcoin_util_a_CPPFLAGS`; `$(LIBUTF8PROC)` on the `LDADD`
   of binaries linking it (Section 6). This is the library exercised during
   wallet creation (`CHDChain::SetMnemonic` -> `CMnemonic::ToSeed`).
3. **Fallback UX — RESOLVED.** Legacy-seed adoption **surfaces a one-time
   notification** (plus a debug-log entry), not a silent switch (Section 8).

Open:

4. **Japanese generation.** Confirm switching `FromData` Japanese output to
   `U+3000` is acceptable for any downstream consumers that currently expect
   ASCII-separated generated phrases (import is unaffected; display/export may be).
