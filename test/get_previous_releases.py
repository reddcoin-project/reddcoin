#!/usr/bin/env python3
#
# Copyright (c) 2018-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Download or build previous releases.
# Needs curl and tar to download a release, or the build dependencies when
# building a release.

import argparse
import contextlib
from fnmatch import fnmatch
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import hashlib


# Checksums of the release archives this script downloads, keyed by hash.
#
# Reddcoin publishes these on download.reddcoin.com under the same layout
# bitcoincore.org uses, bin/reddcoin-core-<version>/. Add an entry per platform
# as each is uploaded; only the hosts actually published need to be here.
#
# The archives are the ones Guix produces, unchanged: contrib/guix/guix-build
# writes guix-build-<version>/output/<host>/reddcoin-<version>-<host>.tar.gz,
# which is already the name and the layout this expects. Pass FORCE_VERSION to
# name it for the release rather than for the commit built. Guix also builds
# Linux hosts against glibc 2.24, which matters here: the job that uses these
# runs in ubuntu:18.04, and a host build wants far newer than it has.
SHA256_SUMS = {
    'f58a9d42b56f0d0392bc0008db2be8b0705c68c9d86d21431f1fdeca52f90e62': 'reddcoin-4.22.9.3-x86_64-linux-gnu.tar.gz',
}


@contextlib.contextmanager
def pushd(new_dir) -> None:
    previous_dir = os.getcwd()
    os.chdir(new_dir)
    try:
        yield
    finally:
        os.chdir(previous_dir)


def release_urls(tag, tarball) -> list:
    """Where to look for a release archive, in order of preference.

    Reddcoin publishes on download.reddcoin.com under bin/reddcoin-core-<version>/,
    which is the layout bitcoincore.org uses, named for the version rather than
    the tag. GitHub release assets are a second source, with their own layout,
    and are worth trying because download.reddcoin.com sits behind a proxy whose
    bot protection turns some CI runners away: this fetch once received a 5552
    byte challenge page with a 200 status where a browser got the archive.

    PREVIOUS_RELEASES_URL replaces the list with a single mirror, using the
    download.reddcoin.com layout.
    """
    version = tag[1:]
    override = os.environ.get('PREVIOUS_RELEASES_URL')
    if override:
        return ['{base}/reddcoin-core-{version}/{tarball}'.format(
            base=override.rstrip('/'), version=version, tarball=tarball)]
    return [
        'https://download.reddcoin.com/bin/reddcoin-core-{version}/{tarball}'.format(
            version=version, tarball=tarball),
        'https://github.com/reddcoin-project/reddcoin/releases/download/{tag}/{tarball}'.format(
            tag=tag, tarball=tarball),
    ]


def describe_payload(tarball) -> str:
    """Summarise what actually arrived, for when it is not the archive.

    A bare "checksum did not match" gives no clue whether the file is truncated,
    a redirect, or an HTML error page, which is the difference between a stale
    checksum and a host that is refusing to serve us.
    """
    size = Path(tarball).stat().st_size
    with open(tarball, 'rb') as f:
        head = f.read(200)
    preview = head.decode('utf-8', 'replace').replace('\n', ' ').replace('\r', '')
    return '{size} bytes, begins: {preview}'.format(
        size=size, preview=preview.strip()[:150])


def download_binary(tag, args) -> int:
    if Path(tag).is_dir():
        if not args.remove_dir:
            print('Using cached {}'.format(tag))
            return 0
        shutil.rmtree(tag)
    Path(tag).mkdir()
    tarball = 'reddcoin-{tag}-{platform}.tar.gz'.format(
        tag=tag[1:], platform=args.platform)

    for tarballUrl in release_urls(tag, tarball):
        print('Fetching: {tarballUrl}'.format(tarballUrl=tarballUrl))

        header, status = subprocess.Popen(
            ['curl', '--location', '--head', tarballUrl],
            stdout=subprocess.PIPE).communicate()
        if re.search("404 Not Found", header.decode("utf-8")):
            print("  not published here")
            continue

        if subprocess.run(
                ['curl', '--location', '--remote-name', tarballUrl]).returncode:
            print("  download failed")
            continue

        hasher = hashlib.sha256()
        with open(tarball, "rb") as afile:
            hasher.update(afile.read())
        tarballHash = hasher.hexdigest()

        if tarballHash not in SHA256_SUMS or SHA256_SUMS[tarballHash] != tarball:
            # Either the archive changed and SHA256_SUMS is stale, or something
            # other than the archive was served. describe_payload says which.
            print("  checksum did not match")
            print("  got sha256 {}".format(tarballHash))
            print("  {}".format(describe_payload(tarball)))
            Path(tarball).unlink()
            continue

        print("Checksum matched")
        break
    else:
        print("No source served a matching {tarball}".format(tarball=tarball))
        return 1

    # Extract tarball
    ret = subprocess.run(['tar', '-zxf', tarball, '-C', tag,
                          '--strip-components=1',
                          'reddcoin-{tag}'.format(tag=tag[1:])]).returncode
    if ret:
        return ret

    Path(tarball).unlink()
    return 0


def build_release(tag, args) -> int:
    githubUrl = "https://github.com/reddcoin-project/reddcoin"
    if args.remove_dir:
        if Path(tag).is_dir():
            shutil.rmtree(tag)
    if not Path(tag).is_dir():
        # fetch new tags
        subprocess.run(
            ["git", "fetch", githubUrl, "--tags"])
        output = subprocess.check_output(['git', 'tag', '-l', tag])
        if not output:
            print('Tag {} not found'.format(tag))
            return 1
    ret = subprocess.run([
        'git', 'clone', githubUrl, tag
    ]).returncode
    if ret:
        return ret
    with pushd(tag):
        ret = subprocess.run(['git', 'checkout', tag]).returncode
        if ret:
            return ret
        host = args.host
        if args.depends:
            with pushd('depends'):
                ret = subprocess.run(['make', 'NO_QT=1']).returncode
                if ret:
                    return ret
                host = os.environ.get(
                    'HOST', subprocess.check_output(['./config.guess']))
        config_flags = '--prefix={pwd}/depends/{host} '.format(
            pwd=os.getcwd(),
            host=host) + args.config_flags
        cmds = [
            './autogen.sh',
            './configure {}'.format(config_flags),
            'make',
        ]
        for cmd in cmds:
            ret = subprocess.run(cmd.split()).returncode
            if ret:
                return ret
        # Move binaries, so they're in the same place as in the
        # release download
        Path('bin').mkdir(exist_ok=True)
        files = ['reddcoind', 'reddcoin-cli', 'reddcoin-tx']
        for f in files:
            Path('src/'+f).rename('bin/'+f)
    return 0


def check_host(args) -> int:
    args.host = os.environ.get('HOST', subprocess.check_output(
        './depends/config.guess').decode())
    if args.download_binary:
        platforms = {
            'aarch64-*-linux*': 'aarch64-linux-gnu',
            'x86_64-*-linux*': 'x86_64-linux-gnu',
            'x86_64-apple-darwin*': 'osx64',
        }
        args.platform = ''
        for pattern, target in platforms.items():
            if fnmatch(args.host, pattern):
                args.platform = target
        if not args.platform:
            print('Not sure which binary to download for {}'.format(args.host))
            return 1
    return 0


def main(args) -> int:
    Path(args.target_dir).mkdir(exist_ok=True, parents=True)
    print("Releases directory: {}".format(args.target_dir))
    ret = check_host(args)
    if ret:
        return ret
    if args.download_binary:
        with pushd(args.target_dir):
            for tag in args.tags:
                ret = download_binary(tag, args)
                if ret:
                    return ret
        return 0
    args.config_flags = os.environ.get('CONFIG_FLAGS', '')
    args.config_flags += ' --without-gui --disable-tests --disable-bench'
    with pushd(args.target_dir):
        for tag in args.tags:
            ret = build_release(tag, args)
            if ret:
                return ret
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('-r', '--remove-dir', action='store_true',
                        help='remove existing directory.')
    parser.add_argument('-d', '--depends', action='store_true',
                        help='use depends.')
    parser.add_argument('-b', '--download-binary', action='store_true',
                        help='download release binary.')
    parser.add_argument('-t', '--target-dir', action='store',
                        help='target directory.', default='releases')
    parser.add_argument('tags', nargs='+',
                        help="release tags. e.g.: v0.18.1 v0.20.0rc2")
    args = parser.parse_args()
    sys.exit(main(args))
