#!/usr/bin/env bash
#
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Copyright (c) 2018-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Native aarch64 build, run on a GitHub-hosted arm64 runner (ubuntu-24.04-arm).
# This replaces the former 32-bit arm-linux-gnueabihf cross job: building and
# testing natively on real ARM hardware means there is no qemu-user wrapper, so
# the Qt GUI test (test_reddcoin-qt) runs as an ordinary process instead of
# aborting in QtTest setup under emulation. debian:bookworm is pulled as its
# arm64 image automatically on the arm64 host (no --platform needed).
export HOST=aarch64-linux-gnu
export PACKAGES="python3-zmq busybox libfontconfig1 libxcb1"
export CONTAINER_NAME=ci_arm64_linux
export DOCKER_NAME_TAG="debian:bookworm"
export USE_BUSY_BOX=true
export RUN_UNIT_TESTS=true
export RUN_FUNCTIONAL_TESTS=false
export GOAL="install"
# -Wno-error=suggest-override: GCC 12 (bookworm) raises -Wsuggest-override on the
# depends-built Boost headers (which the build compiles with
# -Werror=suggest-override), failing the source build even though those are
# third-party headers. Demote it to a warning so only our own code is enforced.
export BITCOIN_CONFIG="--enable-glibc-back-compat --enable-reduce-exports CXXFLAGS=-Wno-error=suggest-override"
