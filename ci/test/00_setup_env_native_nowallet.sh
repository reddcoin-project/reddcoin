#!/usr/bin/env bash
#
# Copyright (c) 2019-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_nowallet
export DOCKER_NAME_TAG=ubuntu:18.04  # Use bionic to have one config run the tests in python3.6, see doc/dependencies.md
export PACKAGES="python3-zmq clang-5.0 llvm-5.0"  # Use clang-5 to test C++17 compatibility, see doc/dependencies.md
export DEP_OPTS="NO_WALLET=1"
export GOAL="install"
export BITCOIN_CONFIG="--enable-glibc-back-compat --enable-reduce-exports CC=clang-5.0 CXX=clang++-5.0"
# The functional suite needs a staked chain: the shared test cache is built with
# createwallet / importprivkey / staked generatetoaddress, and regtest caps PoW
# at nLastPowHeight so any chain past it must be produced by proof-of-stake. A
# --disable-wallet node cannot stake (that is the point of this build: it
# validates PoS blocks but does not produce them), so the cache cannot be built
# here. This job covers the build, the link and the unit tests (make check);
# the functional suite runs in the wallet-enabled jobs.
export RUN_FUNCTIONAL_TESTS=false
