#!/usr/bin/env bash
#
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=x86_64-apple-darwin18
export PIP_PACKAGES="zmq lief"
export GOAL="install"
export BITCOIN_CONFIG="--with-gui --enable-reduce-exports"
export CI_OS_NAME="macos"
export NO_DEPENDS=1
export OSX_SDK=""
export CCACHE_SIZE=300M
# The binary security and symbol checks need a LIEF whose API
# contrib/devtools/security-check.py is written against, and no such version
# has a wheel for the Python this runner ships, so they cannot run here yet.
# The release path is unaffected: guix pins LIEF and runs both checks there.
# Re-enable once RED-144 has made the scripts version-tolerant.
export RUN_SECURITY_TESTS="false"
