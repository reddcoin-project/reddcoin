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
OUT=signature-osx.tar.gz
OUTROOT=osx/dist

if [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; then
  echo "usage: $0 <path to key> [<path to app store connect key> <app store connect issuer id>]"
  echo "example: $0 Certificates.p12 AuthKey_ABCDE12345.p8 01234567-89ab-cdef-0123-456789abcdef"
  echo "With the key alone the bundle is signed but not notarized."
  exit 1
fi

# Apple notarizes only a signature made with the hardened runtime, and the
# signapple releases that can notarize are the ones that can set it.
if ! ${SIGNAPPLE} sign -h | grep -q -- "--hardened-runtime"; then
  echo "This ${SIGNAPPLE} cannot sign with the hardened runtime. Install a version that has the notarize command."
  exit 1
fi

rm -rf ${TEMPDIR} ${NOTARIZE_TEMPDIR}
mkdir -p ${TEMPDIR}

${SIGNAPPLE} sign -f --hardened-runtime --detach "${TEMPDIR}/${OUTROOT}" "$1" "${BUNDLE}"

if [ "$#" -eq 3 ]; then
  # Apple is sent a signed bundle, so attach the signature to a copy and leave
  # the unsigned one as it was unpacked. The ticket that comes back is written
  # beside the detached signature, and is applied along with it.
  mkdir -p ${NOTARIZE_TEMPDIR}
  cp -R "${BUNDLE}" "${NOTARIZE_TEMPDIR}/"
  ${SIGNAPPLE} apply "${NOTARIZE_TEMPDIR}/${BUNDLE_NAME}" "${TEMPDIR}/${OUTROOT}/${BUNDLE_NAME}"
  ${SIGNAPPLE} notarize --detach "${TEMPDIR}/${OUTROOT}" "$2" "$3" "${NOTARIZE_TEMPDIR}/${BUNDLE_NAME}"
  rm -rf ${NOTARIZE_TEMPDIR}
else
  echo "No App Store Connect key given: the signature is not notarized."
fi

tar -C "${TEMPDIR}" -czf "${OUT}" .
rm -rf "${TEMPDIR}"
echo "Created ${OUT}"
