#!/usr/bin/env bash
#
# Copyright (c) 2018-2020 The Bitcoin Core developers
# Copyright (c) 2018-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

# Provider-neutral CI variables, exported by the CI workflow (see
# .github/workflows/ci.yml). Keeping these CI-agnostic means the script does
# not need editing if the CI provider changes again.
#   CI_PR             - non-empty on pull request builds
#   CI_BASE_REF       - PR target branch, used to compute the merge base
#   CI_REPO_FULL_NAME - "owner/repo"
#   RUN_COMMIT_VERIFY - "true" to run the verify-commits cron job
CI_BASE_REF="${CI_BASE_REF:-develop}"

GIT_HEAD=$(git rev-parse HEAD)
# Determine the range of commits under review relative to the base branch and
# export it, so the downstream linters (lint-whitespace, lint-git-commit-check)
# operate on a defined range instead of falling back to a hardcoded base branch
# name (e.g. "master"/"develop") that may not exist in the CI checkout.
COMMIT_RANGE="${COMMIT_RANGE:-}"
if [ -z "$COMMIT_RANGE" ] && git rev-parse -q --verify "origin/${CI_BASE_REF}^{commit}" >/dev/null 2>&1; then
  CI_BASE_SHA=$(git merge-base "origin/${CI_BASE_REF}" "$GIT_HEAD")
  COMMIT_RANGE="$CI_BASE_SHA..$GIT_HEAD"
fi
export COMMIT_RANGE

# commit-script-check only makes sense for the commits a pull request introduces.
if [ -n "$CI_PR" ] && [ -n "$COMMIT_RANGE" ]; then
  test/lint/commit-script-check.sh "$COMMIT_RANGE"
fi

# This only checks that the trees are pure subtrees, it is not doing a full
# check with -r to not have to fetch all the remotes.
test/lint/git-subtree-check.sh src/crypto/ctaes
test/lint/git-subtree-check.sh src/secp256k1
test/lint/git-subtree-check.sh src/univalue
test/lint/git-subtree-check.sh src/leveldb
test/lint/git-subtree-check.sh src/crc32c
test/lint/check-doc.py
test/lint/lint-all.sh

if [ "$CI_REPO_FULL_NAME" = "reddcoin-project/reddcoin" ] && [ "$RUN_COMMIT_VERIFY" = "true" ]; then
    # The trusted keys ship in the repo, so the check does not depend on a
    # keyserver. Merges come from the GitHub merge button, which writes no
    # Tree-SHA512 line, so the tree check is off. The clean-merge check redoes
    # recent merges with git merge, which needs an identity; the container
    # has none.
    gpg --import contrib/verify-commits/trusted-keys.asc &&
    GIT_AUTHOR_NAME=verify-commits GIT_AUTHOR_EMAIL=verify-commits@localhost \
    GIT_COMMITTER_NAME=verify-commits GIT_COMMITTER_EMAIL=verify-commits@localhost \
    ./contrib/verify-commits/verify-commits.py --disable-tree-check --clean-merge=2;
fi

echo
git log --no-merges --oneline $COMMIT_RANGE
