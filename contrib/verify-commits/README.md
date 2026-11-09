Tooling for verification of PGP signed commits
----------------------------------------------

This is an incomplete work in progress, but currently includes a pre-push hook
script (`pre-push-hook.sh`) for maintainers to ensure that their own commits
are PGP signed (nearly always merge commits), as well as a Python 3 script to verify
commits against a trusted keys list.


Using verify-commits.py safely
------------------------------

Remember that you can't use an untrusted script to verify itself. This means
that checking out code, then running `verify-commits.py` against `HEAD` is
_not_ safe, because the version of `verify-commits.py` that you just ran could
be backdoored. Instead, you need to use a trusted version of verify-commits
prior to checkout to make sure you're checking out only code signed by trusted
keys:

 ```sh
 git fetch origin && \
 ./contrib/verify-commits/verify-commits.py origin/master && \
 git checkout origin/master
 ```

Note that the above isn't a good UI/UX yet, and needs significant improvements
to make it more convenient and reduce the chance of errors; pull-reqs
improving this process would be much appreciated.

Configuration files
-------------------

* `trusted-git-root`: This file should contain a single git commit hash which is the first unsigned git commit (hence it is the "root of trust").
* `trusted-sha512-root-commit`: This file should contain a single git commit hash which is the first commit without a SHA512 root commitment.
* `trusted-keys`: This file should contain a \n-delimited list of all PGP fingerprints of authorized commit signers (primary, not subkeys).
* `allow-revsig-commits`: This file should contain a \n-delimited list of git commit hashes. See next section for more info.

Import trusted keys
-------------------
In order to check the commit signatures, you must add the trusted PGP keys to your machine. The public keys ship next to the fingerprint list, so [GnuPG](https://gnupg.org/) can import them without a keyserver:

```sh
gpg --import contrib/verify-commits/trusted-keys.asc
```

Check that the fingerprints gpg prints match `trusted-keys`.

Reddcoin signing policy
-----------------------

Every commit on the first-parent history of `develop` after `trusted-git-root` must carry a valid signature from a key in `trusted-keys`:

* Pull requests are merged with the GitHub merge button, so merge commits are signed by GitHub's web-flow key (`968479A1AFF927E37D1A566BB5690EEEBB952194`). The branch protection on `develop` requires signed commits.
* Commits pushed straight to `develop` must be signed by a maintainer key.

GitHub's merge commits carry no `Tree-SHA512` line, so CI runs the check with `--disable-tree-check`. It runs on the nightly scheduled CI job:

```sh
./contrib/verify-commits/verify-commits.py --disable-tree-check --clean-merge=2
```

To add a maintainer, append the primary key fingerprint to `trusted-keys` and the public key to `trusted-keys.asc`. If GitHub rotates its web-flow key, add the new key the same way, and keep the old one until no commit after `trusted-git-root` depends on it, or list those commits in `allow-revsig-commits`.

Key expiry/revocation
---------------------

When a key (or subkey) which has signed old commits expires or is revoked,
verify-commits will start failing to verify all commits which were signed by
said key. In order to avoid bumping the root-of-trust `trusted-git-root`
file, individual commits which were signed by such a key can be added to the
`allow-revsig-commits` file. That way, the PGP signatures are still verified
but no new commits can be signed by any expired/revoked key. To easily build a
list of commits which need to be added, verify-commits.py can be edited to test
each commit with BITCOIN_VERIFY_COMMITS_ALLOW_REVSIG set to both 1 and 0, and
those which need it set to 1 printed.
