# BIP39 Unicode (NFKD) Normalization Design Document

**Component:** `src/util/bip39` (CMnemonic)
**Last Updated:** 2026-07-18
**Status:** Implemented on branch `bip39/nfkd-normalization` (dual-derivation fallback dropped — see Section 8)

---

## Executive Summary

Reddcoin's BIP39 implementation (`CMnemonic`) does not apply the Unicode NFKD
normalization that BIP39 mandates for the mnemonic sentence and passphrase.
Because every non-ASCII wordlist shipped in the tree is stored in **NFKD** form
and word matching is a raw byte comparison, a **valid** French, Spanish,
Japanese, or Korean mnemonic entered in the more common **NFC** form (what
macOS, iOS, and most IMEs emit) fails validation today, and mnemonics/passphrases
that are not already normalized derive a non-interoperable seed.

The interim ideographic-space fix (U+3000 -> U+0020) addressed one narrow
instance of this defect for Japanese. This document describes the general,
spec-compliant fix as implemented: vendor `utf8proc` as a `src/` subtree,
normalize the mnemonic and passphrase with NFKD at every parse and derivation
point, and switch Japanese mnemonic generation to the canonical ideographic-space
separator. A dual-derivation fallback was designed and then dropped; the
rationale is in Section 8.

**Key outcomes:**
- Valid fr/es/ja/ko mnemonics validate regardless of input normalization form.
- Seeds match upstream BIP39 wallets (and ecosystem tooling such as
  bitcore-mnemonics) for non-ASCII mnemonics and passphrases.
- No change to ASCII-only wallets (NFKD is a no-op on ASCII).

---

## Table of Contents

1. Problem Statement
2. Goals and Non-Goals
3. Background: BIP39 Normalization Requirements
4. Design Overview
5. Dependency: Vendoring utf8proc
6. Build Integration
7. Detailed Changes
8. Dual-Derivation Fallback (Considered and Dropped)
9. Backward Compatibility and Migration
10. Test Plan
11. Security Considerations
12. Rollout and Effort
13. Alternatives Considered
14. Resolved Decisions

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

### Non-Goals
- Adding new BIP39 languages or changing wordlists.
- Changing key derivation for ASCII-only wallets (NFKD is a no-op there and no
  behavior changes).
- A dual-derivation (NFKD vs legacy) fallback on restore. Considered and dropped;
  see Section 8.
- Any wallet-code, keypool, rescan, or UI changes. The change is confined to
  `CMnemonic` and its build wiring.
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
- **`ToSeed`:** normalize the mnemonic and the whole salt string:
  ```cpp
  mnemonic = NormalizeNFKD(mnemonic);
  SecureString ssSalt = NormalizeNFKD(SecureString("mnemonic") + passphrase);
  ```
  This matches BIP39 exactly (`salt = NFKD("mnemonic" + passphrase)`); the salt
  is normalized as a whole string, not by normalizing the passphrase separately.
- **`FromData` (generation):** join Japanese words with `U+3000` instead of
  ASCII space. All other languages keep ASCII space. Output remains NFKD because
  the wordlists are NFKD. This is cosmetic for interop (import re-normalizes) but
  makes generated Japanese mnemonics spec-canonical.

### 7.2 Scope of the change

The change is confined to `src/util/bip39.{cpp,h}` and the build wiring. No
signature changes, no wallet-code, keypool, rescan, HD-chain-serialization, or UI
changes. `CHDChain::SetMnemonic` (`src/wallet/walletdb.cpp:68`) calls `ToSeed`
unchanged and transparently benefits from the normalization.

---

## 8. Dual-Derivation Fallback (Considered and Dropped)

A dual-derivation fallback was designed to protect wallets created under the old,
non-normalized code: on restore, derive both the NFKD seed and a "legacy"
(non-normalized) seed, rescan for both, and adopt whichever holds on-chain
history. It was **dropped** after weighing cost against the actual at-risk
population.

### Why it was dropped

- **The legacy population is near-empty.** Under the pre-change `Check`, word
  matching was byte-exact against the NFKD wordlists, so an NFC mnemonic (the
  common case) could not validate and such a wallet could not be created in Core
  at all. The only wallets whose seed actually changes under NFKD are those
  created with an already-NFKD mnemonic **and** a non-ASCII, non-NFKD
  *passphrase* - a vanishingly small set.
- **Ecosystem tooling was already NFKD-correct.** The canonical mnemonic library
  in the Reddcoin/BitPay ecosystem (`bitcore-mnemonics`) already performs NFKD
  (`unorm.nfkd` / native `normalize('NFKD')`), so seeds produced by ecosystem
  wallets already match the post-change Core derivation. There is no ecosystem
  "legacy" seed to recover.
- **The cost/risk is disproportionate.** `createwallet` is a create-not-restore
  operation with no natural rescan hook; true auto-adoption would require deriving
  lookahead scripts for both seeds, scanning the chain against both sets, and
  rebuilding the keypool from the winner - a large, delicate change to core
  keypool/rescan code and the HD-chain serialization, for that near-empty set.

### Consequence

- New wallets always use canonical NFKD (there is no other mode).
- ASCII-only wallets are unaffected (NFKD is a no-op), which is the overwhelming
  majority.
- A user who genuinely created a pre-change Core wallet with a non-ASCII
  passphrase and finds it empty after restore can recover it with the old binary;
  this is documented in the release notes rather than automated. If demand for an
  in-product path ever materializes, the smallest sufficient addition is an
  opt-in `legacy_derivation` flag on `createwallet` (no rescan machinery, no
  serialization change) - deliberately left as a possible follow-up, not built
  here.

---

## 9. Backward Compatibility and Migration

- **ASCII mnemonic + ASCII passphrase:** `NFKD(x) == x`. Seeds are byte-for-byte
  identical. English and Italian wallets, and every all-ASCII wallet in any
  language, are unaffected. This is the overwhelming majority of wallets.
- **Non-ASCII wallets created before this change:** the only wallets whose seed
  changes are those created in Core with an already-NFKD mnemonic and a
  non-ASCII, non-NFKD passphrase (Section 8) - a near-empty set. No automated
  fallback is provided; recovery via the old binary is documented in the release
  notes.
- **Interim ideographic-space fix:** its Japanese seed behavior is a strict
  subset of NFKD, so anything validated under it derives the same canonical seed
  here. Its vectors and the space-normalization test are folded into the main
  `japanese` section and retained (Section 10).
- **Chinese:** CJK ideographs in the Chinese wordlists are NFKD-stable, so
  Chinese wallets are unaffected in practice.

---

## 10. Test Plan

Implemented in `src/test/bip39_tests.cpp` and `src/test/data/bip39_vectors.json`:

1. **NFC-input regression (the headline fix).** `bip39_nfc_input`: for French,
   Spanish, and Korean, compose a valid vector mnemonic to NFC (via utf8proc) and
   assert `Check` passes and `ToSeed` equals the canonical vector seed. These
   cases fail before the change and demonstrate the real-world defect.
2. **NFKD passphrase vectors.** `bip39_nfkd_passphrase`: the canonical bip32JP /
   `test_JP_BIP39` Japanese vectors (NFC mnemonics, passphrase
   `㍍ガバヴァぱばぐゞちぢ十人十色`), added under a `japanese_nfkd` key. They
   exercise NFKD of both the mnemonic and the passphrase.
3. **Ideographic-space / mixed separators.** `bip39_japanese_ideographic_space`
   over the `japanese` section (now `U+3000`), checking the ideographic, ASCII,
   and mixed forms all validate and derive the same seed.
4. **Malformed UTF-8.** `bip39_malformed_utf8`: invalid UTF-8 degrades gracefully
   (no crash, 64-byte seed, `Check` returns false).
5. **Existing vectors unchanged.** `bip39_vectors` / `bip44_tests` still pass;
   ASCII seeds are byte-identical (NFKD no-op), and the Japanese sections were
   realigned to `U+3000` to match canonical generation (seeds/keys unchanged).

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
  a given (mnemonic, passphrase) is stable across platforms.
- **Consensus.** None. This is wallet-local key derivation; it does not touch
  block or transaction validation.

---

## 12. Rollout

Landed as an independently-reviewable commit series on `bip39/nfkd-normalization`:

1. **Subtree** — vendor `src/utf8proc/` v2.9.0.
2. **Build** — `Makefile.utf8proc.include`, link wiring, `AC_PROG_CC`.
3. **Normalization core** — `NormalizeNFKD` (secure-memory), wired into the parse
   and derive functions; whole-salt normalization.
4. **Design doc** (this document).
5. **Tests + vectors + `U+3000` generation** — Section 10.

The dual-derivation fallback (former phase) was dropped (Section 8), so no
wallet-code, keypool, rescan, or serialization work is part of this change.

---

## 13. Alternatives Considered

- **ICU.** Full-featured but heavyweight; a large new dependency surface that
  Bitcoin Core intentionally avoids. Rejected.
- **Hand-rolled NFKD subset.** Table covering only the codepoints in the
  wordlists. Rejected: cannot correctly normalize arbitrary passphrases, and
  shipping a partial Unicode table in seed-derivation code is fragile and
  security-sensitive.
- **Space-normalization only (the interim fix).** Correct but partial; does
  not fix NFC accents, decomposed jamo, or passphrase normalization. Folded into
  this change as a subset.
- **Dual-derivation fallback (auto rescan-adopt / opt-in flag).** Designed and
  dropped; the at-risk population is near-empty and ecosystem tooling was already
  NFKD-correct (Section 8). Chosen path: document-only migration, with an opt-in
  `legacy_derivation` flag left as a possible follow-up if demand appears.

---

## 14. Resolved Decisions

1. **utf8proc pin — RESOLVED.** Pin to utf8proc **v2.9.0**. Record the tarball
   SHA256 in the subtree import commit for provenance.
2. **Library placement — RESOLVED.** `util/bip39.cpp` compiles into
   **`libbitcoin_util`** (`libbitcoin_util_a_SOURCES`, `src/Makefile.am:621`).
   Include flags on `libbitcoin_util_a_CPPFLAGS`; `$(LIBUTF8PROC)` on the `LDADD`
   of binaries linking it (Section 6). This is the library exercised during
   wallet creation (`CHDChain::SetMnemonic` -> `CMnemonic::ToSeed`).
3. **Dual-derivation fallback — RESOLVED (dropped).** Not implemented; the
   at-risk population is near-empty and ecosystem tooling was already
   NFKD-correct (Section 8). New wallets are always canonical NFKD.
4. **Japanese generation — RESOLVED.** `FromData` emits `U+3000` for Japanese.
   Import re-normalizes either separator, so only generated/displayed phrases are
   affected; the bip39/bip44 Japanese vectors were realigned accordingly.
