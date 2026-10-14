#!/bin/sh
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Copyright (c) 2014-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
set -e

ROOTDIR=dist
BUNDLE_NAME="Reddcoin-Qt.app"
BUNDLE="${ROOTDIR}/${BUNDLE_NAME}"
SIGNAPPLE=signapple
TEMPDIR=sign.temp
NOTARIZE_TEMPDIR=notarize.temp

if [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; then
  echo "usage: $0 <path to key> [<path to app store connect key> <app store connect issuer id>]"
  echo "example: $0 Certificates.p12 AuthKey_ABCDE12345.p8 01234567-89ab-cdef-0123-456789abcdef"
  echo "With the key alone everything is signed but not notarized."
  exit 1
fi

# Apple notarizes only a signature made with the hardened runtime, and the
# signapple releases that can notarize are the ones that can set it.
if ! ${SIGNAPPLE} sign -h | grep -q -- "--hardened-runtime"; then
  echo "This ${SIGNAPPLE} cannot sign with the hardened runtime. Install a version that has the notarize command."
  exit 1
fi

ARCH=$(${SIGNAPPLE} info "${BUNDLE}/Contents/MacOS/Reddcoin-Qt" | head -n 1 | cut -d " " -f 1)
if [ -z "${ARCH}" ]; then
  echo "Could not read the architecture of ${BUNDLE}"
  exit 1
fi

# The x86_64 and arm64 signatures share one detached-sigs repository, and the
# app bundle signatures would overwrite each other, so each architecture gets
# its own directory and its own output tarball.
OUT="signature-osx-${ARCH}.tar.gz"
OUTROOT="osx/${ARCH}-apple-darwin"

# The standalone binaries and the shared library, as paths relative to here.
# A symlink to a library is skipped, the library it names is already listed.
BINARIES=""
for bin in reddcoin-*/bin/* reddcoin-*/lib/*.dylib; do
  if [ -f "${bin}" ] && [ ! -L "${bin}" ]; then
    BINARIES="${BINARIES} ${bin}"
  fi
done

rm -rf ${TEMPDIR} ${NOTARIZE_TEMPDIR}
mkdir -p ${TEMPDIR}

# Every file is signed with the same key, so ask for its passphrase once
# rather than once per file.
if [ -t 0 ]; then
  trap 'stty echo' EXIT
  stty -echo
fi
printf "Enter the passphrase for %s: " "$1"
read -r cs_key_pass
printf "\n"
if [ "$#" -eq 3 ]; then
  printf "Enter the passphrase for %s (empty if it has none): " "$2"
  read -r api_key_pass
  printf "\n"
fi
if [ -t 0 ]; then
  stty echo
  trap - EXIT
fi

${SIGNAPPLE} sign -f --hardened-runtime --detach "${TEMPDIR}/${OUTROOT}/${ROOTDIR}" --passphrase "${cs_key_pass}" "$1" "${BUNDLE}"

for bin in ${BINARIES}; do
  ${SIGNAPPLE} sign -f --hardened-runtime --detach "${TEMPDIR}/${OUTROOT}/$(dirname "${bin}")" --passphrase "${cs_key_pass}" "$1" "${bin}"
done

if [ "$#" -eq 3 ]; then
  # Apple is sent signed files, so attach the signatures to copies and leave
  # the unsigned ones as they were unpacked.
  mkdir -p ${NOTARIZE_TEMPDIR}

  # The ticket for the bundle is written beside its detached signature, and
  # is applied along with it.
  cp -R "${BUNDLE}" "${NOTARIZE_TEMPDIR}/"
  ${SIGNAPPLE} apply "${NOTARIZE_TEMPDIR}/${BUNDLE_NAME}" "${TEMPDIR}/${OUTROOT}/${BUNDLE}"
  ${SIGNAPPLE} notarize --detach "${TEMPDIR}/${OUTROOT}/${ROOTDIR}" --passphrase "${api_key_pass}" "$2" "$3" "${NOTARIZE_TEMPDIR}/${BUNDLE_NAME}"

  # A ticket cannot be attached to a plain binary. Notarizing these registers
  # them with Apple, and adds nothing to the detached signature.
  if [ -n "${BINARIES}" ]; then
    for bin in ${BINARIES}; do
      mkdir -p "${NOTARIZE_TEMPDIR}/binaries/$(dirname "${bin}")"
      cp "${bin}" "${NOTARIZE_TEMPDIR}/binaries/${bin}"
      ${SIGNAPPLE} apply "${NOTARIZE_TEMPDIR}/binaries/${bin}" "${TEMPDIR}/${OUTROOT}/${bin}.${ARCH}sign"
    done
    ${SIGNAPPLE} notarize --passphrase "${api_key_pass}" "$2" "$3" "${NOTARIZE_TEMPDIR}/binaries"
  fi
  rm -rf ${NOTARIZE_TEMPDIR}
else
  echo "No App Store Connect key given: the signatures are not notarized."
fi

tar -C "${TEMPDIR}" -czf "${OUT}" .
rm -rf "${TEMPDIR}"
echo "Created ${OUT}"
