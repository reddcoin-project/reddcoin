#!/usr/bin/env bash
#
# Copyright (c) 2020 The Bitcoin Core developers
# Copyright (c) 2020-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=i686-pc-linux-gnu
export CONTAINER_NAME=ci_i686_centos
# stream8 reached EOL: mirrorlist.centos.org / mirror.centos.org no longer
# resolve, so dnf cannot fetch any repo metadata. stream9 is current and ships
# gcc 11 (matching the local dev toolchain). Follows upstream Bitcoin Core
# bitcoin#27662, which bumped this image stream8 -> stream9: python3-zmq is not
# packaged for stream9, so pull the zmq bindings via pip (pyzmq) instead, and add
# python3-pip + util-linux. TEST_RUNNER_ENV's en_US.UTF-8 is dropped because that
# locale is not generated in the stream9 image; the tests run under C.UTF-8.
# The stream9 base perl is minimal and splits several core modules into their own
# packages; the depends OpenSSL 1.1.1 ./Configure and build need FindBin,
# IPC::Cmd, Data::Dumper and File::Copy/Compare, so pull those in explicitly
# (otherwise Configure dies with "Can't locate FindBin.pm in @INC").
export DOCKER_NAME_TAG=quay.io/centos/centos:stream9
export DOCKER_PACKAGES="gcc-c++ glibc-devel.x86_64 libstdc++-devel.x86_64 glibc-devel.i686 libstdc++-devel.i686 ccache libtool make git python3 python3-pip which patch lbzip2 xz procps-ng dash rsync coreutils bison util-linux perl-FindBin perl-IPC-Cmd perl-Data-Dumper perl-File-Compare perl-File-Copy"
export PIP_PACKAGES="pyzmq"
export GOAL="install"
export BITCOIN_CONFIG="--enable-zmq --with-gui=qt5 --enable-reduce-exports"
export CONFIG_SHELL="/bin/dash"
# Don't build with -Werror, matching upstream's stream9 i686 task. gcc 11
# (unlike the stream8 gcc 8) enables -Wsuggest-override, and the depends boost
# 1.71 headers reach the compile via the plain -I depends include rather than
# the -isystem BOOST_CPPFLAGS, so their unmarked virtual overrides would
# otherwise fail the build with -Werror=suggest-override.
export NO_WERROR=1
