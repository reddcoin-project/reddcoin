#!/usr/bin/env bash
#
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Native build on an Apple Silicon (arm64) runner, against Homebrew. The same
# as 00_setup_env_mac_host.sh except where arm64 differs.
export HOST=arm64-apple-darwin
export CONTAINER_NAME=ci_macos_native_arm64
export PIP_PACKAGES="zmq"
export GOAL="install"
# Homebrew lives in /opt/homebrew on arm64, which the compiler does not search
# by default (/usr/local, Homebrew's Intel prefix, it does). Headers and
# libraries found without pkg-config, such as miniupnpc and libnatpmp, need it
# passed in.
export BITCOIN_CONFIG="--with-gui --enable-reduce-exports CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib"
# Homebrew no longer has Berkeley DB 4.8 for arm64 (berkeley-db@4 was disabled
# on 2025-08-24), so the job builds it with contrib/install_db4.sh into db4/ at
# the root of the checkout. configure reads these two from the environment.
export BDB_CFLAGS="-I${BASE_ROOT_DIR}/db4/include"
export BDB_LIBS="-L${BASE_ROOT_DIR}/db4/lib -ldb_cxx-4.8"
export CI_OS_NAME="macos"
export NO_DEPENDS=1
export OSX_SDK=""
export CCACHE_SIZE=300M
# The arm64 runner has 3 cores and 7 GB of memory, half the Intel runner's.
# At the default -j4 the runner lost contact with GitHub about 70 minutes into
# the CI script, most likely from memory pressure, so run fewer jobs at once.
export MAKEJOBS="-j3"
export TEST_RUNNER_JOBS="-j2"
# security-check.py runs the x86_64 Mach-O checks on every Mach-O binary, and
# PIE, NX and CONTROL_FLOW (endbr64) do not apply to arm64. Turn this on once
# the arm64 check list from RED-152 is in.
export RUN_SECURITY_TESTS="false"
