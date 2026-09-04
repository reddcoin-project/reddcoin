#!/usr/bin/env python3
# Copyright (c) 2014-2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sign and verify a release manifest with the offline release key.

The client verifies releases with a BIP340 Schnorr signature over SHA256SUMS,
checked against a public key compiled into the binary. This produces that
signature. It is meant to run on the air-gapped machine that holds the private
key, so it depends on nothing outside the Python standard library and this
repository.

The signing key never leaves that machine. Only SHA256SUMS goes in and only
SHA256SUMS.sig comes out.

Subcommands:

    selftest   run the official BIP340 test vectors
    generate   create a new release key and print its mnemonic
    pubkey     print the x-only public key for a mnemonic
    sign       sign a manifest
    verify     check a signature against a manifest and a public key

Run selftest first on any machine you have not used before. It proves the
implementation on that machine reproduces the BIP340 vectors, which is worth
knowing before it signs something users will trust.
"""

import argparse
import hashlib
import hmac
import os
import re
import secrets
import sys
import unicodedata

# The BIP340 implementation, the curve order and the official test vectors all
# already live in the test framework. Reusing them beats a second copy of
# signature code that nothing else exercises.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', '..', 'test', 'functional'))
from test_framework.key import (  # noqa: E402
    SECP256K1_ORDER,
    compute_xonly_pubkey,
    sign_schnorr,
    verify_schnorr,
)

# Domain separation. The release key is on the same curve as transaction keys,
# so a signature over a bare SHA256 could be meaningful in another context. The
# tag is part of the format: changing it invalidates every signature any
# released client will accept.
MANIFEST_TAG = "Reddcoin/ReleaseManifest"

# BIP32 path the release key is derived at.
#
#   m/1018'/4'/0'
#     │      │   └── index, so a rotated key is 1' from the same backed-up seed
#     │      └────── SLIP-0044 coin type 4, Reddcoin (chainparams nExtCoinType)
#     └───────────── purpose
#
# Deliberately not a wallet path. Reddcoin wallets derive under m/44'/4'/..., and
# a release key that could collide with a spending key is a bad idea however
# unlikely the collision. This path exists for signing releases and nothing else.
#
# On the purpose value: 1018 was checked against the BIP43 registry (44, 45, 47,
# 48, 49, 84, 86, 87) and against known unregistered users, and had no claimant.
# Its neighbour 1017 does: lnd derives every Lightning key under
# m/1017'/coinType'/keyFamily'/0/index, so that prefix would have described this
# key as a Lightning multisig branch. Nothing would have collided, since this
# seed derives nothing else, but a path is read by people and should not say
# something untrue. Absence of a claimant is not a reservation; the REP is what
# records this one.
#
# Fixed once a key has been generated: the mnemonic alone does not identify the
# key without it, so changing the path is changing the key.
DERIVATION_PATH = "m/1018'/4'/0'"


def tagged_hash(tag, data):
    """BIP340 tagged hash. Matches TaggedHash() in src/hash.h."""
    ss = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(ss + ss + data).digest()


# Where the client keeps the English BIP39 wordlist. It sits in src/util/lang on
# the development line and in src/wallet on the 4.22.9 release line, so both are
# tried: this tool is copied to an air-gapped machine and should be one file that
# works from either checkout rather than two that can drift apart.
WORDLIST_PATHS = (
    ('src', 'util', 'lang', 'bip39_english.h'),
    ('src', 'wallet', 'bip39_english.h'),
)


def load_wordlist():
    """The English BIP39 wordlist, read from the header the client uses.

    Parsed rather than duplicated so there is one wordlist in the repository. A
    second copy here could drift from the one the wallet derives with, and the
    divergence would only ever show up as a key that does not match.
    """
    root = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', '..')
    tried = [os.path.join(root, *parts) for parts in WORDLIST_PATHS]
    found = [p for p in tried if os.path.exists(p)]
    if not found:
        raise SystemExit("no BIP39 wordlist found, looked in:\n  " + "\n  ".join(tried))

    path = found[0]
    words = re.findall(r'^\s*"([a-z]+)",?\s*$', open(path, encoding='utf8').read(), re.MULTILINE)
    if len(words) != 2048:
        raise SystemExit("expected 2048 words in {}, found {}".format(path, len(words)))
    return words


def check_mnemonic(mnemonic):
    """Reject a mnemonic that is not a valid BIP39 phrase.

    Worth doing rather than trusting the operator to eyeball the public key
    afterwards. A single mistyped word produces a perfectly usable key for a
    completely different wallet, so without this the tool would happily sign a
    release with a key nothing has ever seen, and the only thing standing
    between that and publishing it would be someone comparing 64 hex
    characters by eye.
    """
    words = unicodedata.normalize("NFKD", mnemonic).split()
    if len(words) not in (12, 15, 18, 21, 24):
        raise SystemExit("mnemonic has {} words, expected 12, 15, 18, 21 or 24".format(len(words)))

    wordlist = load_wordlist()
    index = {w: i for i, w in enumerate(wordlist)}
    unknown = [w for w in words if w not in index]
    if unknown:
        raise SystemExit("not BIP39 words: {}".format(", ".join(sorted(set(unknown)))))

    bits = "".join(format(index[w], "011b") for w in words)
    entropy_bits = len(words) * 32 // 3
    entropy = int(bits[:entropy_bits], 2).to_bytes(entropy_bits // 8, "big")
    expected = format(hashlib.sha256(entropy).digest()[0], "08b")[:len(words) // 3]
    if bits[entropy_bits:] != expected:
        raise SystemExit(
            "mnemonic checksum is wrong. One or more words are mistyped or out of order; "
            "this is not the phrase you think it is")


def mnemonic_from_entropy(entropy):
    """BIP39 mnemonic for a block of entropy.

    The inverse of the checksum test in check_mnemonic: append the first
    len(entropy)/4 bits of SHA256(entropy), then read the result 11 bits at a
    time as indices into the wordlist.
    """
    if len(entropy) not in (16, 20, 24, 28, 32):
        raise SystemExit("entropy must be 16, 20, 24, 28 or 32 bytes, got {}".format(len(entropy)))

    checksum_bits = len(entropy) * 8 // 32
    bits = "".join(format(b, "08b") for b in entropy)
    bits += format(hashlib.sha256(entropy).digest()[0], "08b")[:checksum_bits]

    wordlist = load_wordlist()
    return " ".join(wordlist[int(bits[i:i + 11], 2)] for i in range(0, len(bits), 11))


def bip39_seed(mnemonic, passphrase=""):
    """BIP39 mnemonic to 64-byte seed."""
    mnemonic = unicodedata.normalize("NFKD", " ".join(mnemonic.split()))
    salt = unicodedata.normalize("NFKD", "mnemonic" + passphrase)
    return hashlib.pbkdf2_hmac("sha512", mnemonic.encode(), salt.encode(), 2048)


def bip32_master(seed):
    """BIP32 master key and chain code from a seed.

    "Bitcoin seed" is the HMAC key BIP32 fixes for this step, in every
    implementation and for every coin. It is not a name to localise: a coin
    identifies itself in the derivation path, and changing this constant would
    derive a different key from the same words while looking perfectly correct.
    Keeping it standard is also what lets any BIP39 tool recover the release key
    from the mnemonic, rather than this script being the only thing that can.

    Matches CExtKey::SetSeed() in src/key.cpp, which spells the same constant as
    a char array.
    """
    i = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    return i[:32], i[32:]


def bip32_derive_hardened(privkey, chaincode, index):
    assert index >= 0x80000000, "this only derives hardened children"
    data = b"\x00" + privkey + index.to_bytes(4, "big")
    i = hmac.new(chaincode, data, hashlib.sha512).digest()
    child = (int.from_bytes(i[:32], "big") + int.from_bytes(privkey, "big")) % SECP256K1_ORDER
    if child == 0 or int.from_bytes(i[:32], "big") >= SECP256K1_ORDER:
        raise SystemExit("derivation produced an invalid key, which should not happen")
    return child.to_bytes(32, "big"), i[32:]


def derive_release_key(mnemonic, passphrase=""):
    """Private key at DERIVATION_PATH for a mnemonic."""
    check_mnemonic(mnemonic)
    privkey, chaincode = bip32_master(bip39_seed(mnemonic, passphrase))
    for element in DERIVATION_PATH.split("/")[1:]:
        assert element.endswith("'"), "every path element must be hardened"
        privkey, chaincode = bip32_derive_hardened(privkey, chaincode, int(element[:-1]) + 0x80000000)
    return privkey


def read_text(path):
    with open(path, "r", encoding="utf8") as f:
        return f.read().strip()


def xonly_pubkey(privkey):
    pubkey, _negated = compute_xonly_pubkey(privkey)
    if pubkey is None:
        raise SystemExit("could not compute a public key from that mnemonic")
    return pubkey


def cmd_selftest(_args):
    """Run the official BIP340 vectors against this machine's Python."""
    import csv
    vectors = os.path.join(os.path.dirname(os.path.realpath(__file__)),
                           '..', '..', 'test', 'functional', 'test_framework',
                           'bip340_test_vectors.csv')
    checked = 0
    with open(vectors, newline='', encoding='utf8') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            (_i, seckey, pubkey, aux, msg, sig, result, _comment) = row
            pubkey = bytes.fromhex(pubkey)
            msg = bytes.fromhex(msg)
            sig = bytes.fromhex(sig)
            if seckey:
                seckey = bytes.fromhex(seckey)
                actual_pubkey = xonly_pubkey(seckey)
                if actual_pubkey != pubkey:
                    raise SystemExit("BIP340 vector failed: public key mismatch")
                actual_sig = sign_schnorr(seckey, msg, aux=bytes.fromhex(aux))
                if actual_sig != sig:
                    raise SystemExit("BIP340 vector failed: signature mismatch")
            expected = (result == "TRUE")
            if verify_schnorr(pubkey, sig, msg) != expected:
                raise SystemExit("BIP340 vector failed: verification mismatch")
            checked += 1
    print("BIP340 self-test passed: {} vectors".format(checked))
    return 0


def cmd_generate(args):
    """Create a release key and print the words that reproduce it.

    Deliberately writes nothing to disk. A file holding the mnemonic is a file
    that gets backed up, synced, or left behind on a machine that was supposed
    to be wiped, and this tool is not in a position to know which. Transcribe
    the words onto paper, then type them into a file when there is a manifest to
    sign, and remove that file afterwards.
    """
    if args.entropy_hex:
        # For entropy generated off the machine: dice, coin flips, a hardware
        # source. Worth supporting for a key with no expiry, where trusting one
        # CSPRNG on one box is a choice rather than a default.
        try:
            entropy = bytes.fromhex(args.entropy_hex.strip())
        except ValueError:
            raise SystemExit("--entropy-hex is not valid hexadecimal")
        expected = args.words * 32 // 24
        if len(entropy) != expected:
            raise SystemExit(
                "a {}-word mnemonic needs {} bytes of entropy, got {}".format(
                    args.words, expected, len(entropy)))
        source = "supplied with --entropy-hex"
    else:
        entropy = secrets.token_bytes(args.words * 32 // 24)
        source = "secrets.token_bytes, the platform CSPRNG"

    mnemonic = mnemonic_from_entropy(entropy)

    # Prove the words reproduce a key before showing them to anyone. A
    # generation bug that produced an unusable phrase would otherwise surface
    # months later, when the backup is the only copy left.
    check_mnemonic(mnemonic)
    pubkey = xonly_pubkey(derive_release_key(mnemonic))

    words = mnemonic.split()
    print("Release key, {} words, entropy from {}.\n".format(len(words), source))
    for row in range(0, len(words), 3):
        print("   " + "".join("{:>3}. {:<12}".format(row + col + 1, words[row + col])
                              for col in range(min(3, len(words) - row))))
    print()
    print("path        {}".format(DERIVATION_PATH))
    print("public key  {}".format(pubkey.hex()))
    print()
    print("Write the words down now, on paper, and keep at least one copy away from")
    print("this machine. Nothing was saved to disk. There is no delegation chain and")
    print("no recovery other than these words: losing them ends the ability to sign")
    print("for every client that ships the public key above.")
    print()
    print("Then check the recovery works before trusting it, from the written copy")
    print("rather than from this screen:")
    print()
    print("    {} pubkey --mnemonic <file>".format(os.path.basename(__file__)))
    print()
    print("It must print the same public key. A backup that has never been restored")
    print("is a hypothesis.")
    return 0


def cmd_pubkey(args):
    privkey = derive_release_key(read_text(args.mnemonic),
                                 read_text(args.passphrase) if args.passphrase else "")
    print(xonly_pubkey(privkey).hex())
    return 0


def cmd_sign(args):
    # Read as bytes, never as text. The signature covers the file exactly as it
    # will be published, and any newline or encoding normalisation on the way
    # through would sign something the server does not serve.
    with open(args.manifest, "rb") as f:
        manifest = f.read()

    privkey = derive_release_key(read_text(args.mnemonic),
                                 read_text(args.passphrase) if args.passphrase else "")
    pubkey = xonly_pubkey(privkey)
    sig = sign_schnorr(privkey, tagged_hash(MANIFEST_TAG, manifest))

    # Never emit a signature this tool cannot itself verify.
    if not verify_schnorr(pubkey, sig, tagged_hash(MANIFEST_TAG, manifest)):
        raise SystemExit("refusing to write a signature that does not verify")

    out = args.output or (args.manifest + ".sig")
    with open(out, "w", encoding="utf8") as f:
        f.write(sig.hex() + "\n")

    print("signed   {} ({} bytes)".format(args.manifest, len(manifest)))
    print("public   {}".format(pubkey.hex()))
    print("wrote    {}".format(out))
    print()
    print("Check the public key above against the one in doc/release-process.md")
    print("before publishing. A signature from the wrong key verifies against")
    print("itself and against nothing a released client will accept.")
    return 0


def cmd_verify(args):
    with open(args.manifest, "rb") as f:
        manifest = f.read()
    sig = bytes.fromhex(read_text(args.signature))
    pubkey = bytes.fromhex(args.pubkey)

    if len(sig) != 64:
        raise SystemExit("signature is {} bytes, expected 64".format(len(sig)))
    if len(pubkey) != 32:
        raise SystemExit("public key is {} bytes, expected 32".format(len(pubkey)))

    if not verify_schnorr(pubkey, sig, tagged_hash(MANIFEST_TAG, manifest)):
        print("BAD signature", file=sys.stderr)
        return 1
    print("Good signature over {} ({} bytes)".format(args.manifest, len(manifest)))
    print("by public key {}".format(pubkey.hex()))
    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("selftest", help="run the official BIP340 test vectors")

    p = sub.add_parser("generate", help="create a new release key and print its mnemonic")
    p.add_argument("--words", type=int, default=24, choices=[12, 15, 18, 21, 24],
                   help="mnemonic length (default: 24, for 256 bits of entropy)")
    p.add_argument("--entropy-hex",
                   help="use this entropy instead of the platform CSPRNG, for dice or "
                        "another off-machine source")

    p = sub.add_parser("pubkey", help="print the x-only public key for a mnemonic")
    p.add_argument("--mnemonic", required=True, help="file holding the BIP39 mnemonic")
    p.add_argument("--passphrase", help="file holding the BIP39 passphrase, if any")

    p = sub.add_parser("sign", help="sign a manifest")
    p.add_argument("manifest", help="the SHA256SUMS file that will be published")
    p.add_argument("--mnemonic", required=True, help="file holding the BIP39 mnemonic")
    p.add_argument("--passphrase", help="file holding the BIP39 passphrase, if any")
    p.add_argument("--output", help="signature path (default: <manifest>.sig)")

    p = sub.add_parser("verify", help="check a signature against a manifest")
    p.add_argument("manifest")
    p.add_argument("signature")
    p.add_argument("--pubkey", required=True, help="x-only public key, 64 hex characters")

    args = parser.parse_args()
    return {
        "selftest": cmd_selftest,
        "generate": cmd_generate,
        "pubkey": cmd_pubkey,
        "sign": cmd_sign,
        "verify": cmd_verify,
    }[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
