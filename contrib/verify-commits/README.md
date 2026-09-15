Tooling for verification of PGP signed commits
----------------------------------------------

Status: disabled for ConnectCoin
--------------------------------

ConnectCoin does not yet have a project-owned trusted Git root, Tree-SHA512
root, or authorized signer-key set. The data files in this directory are
inherited Bitcoin Core history and are **not** a ConnectCoin trust policy.
`verify-commits.py` therefore refuses to run while the
`connectcoin-policy-disabled` marker exists.

Do not remove that marker until the project has selected and independently
documented its maintainers, signing keys, trusted roots, review process, and
key rotation/revocation procedure, and every trust file below has been replaced
with ConnectCoin-owned values.

This directory retains the upstream Python verifier and its supporting data as
a starting point for a future ConnectCoin policy. It is incomplete and must not
be treated as an active security control while the disable marker exists.


Enabling verify-commits.py safely
---------------------------------

Remember that you can't use an untrusted script to verify itself. This means
that checking out code, then running `verify-commits.py` against `HEAD` is
_not_ safe, because the version of `verify-commits.py` that you just ran could
be backdoored. Instead, you need to use a trusted version of verify-commits
prior to checkout to make sure you're checking out only code signed by trusted
keys. The exact trusted remote and branch must be part of the future ConnectCoin
policy; the inherited `origin/master` example is deliberately not presented as
an active command.

Note that the above isn't a good UI/UX yet, and needs significant improvements
to make it more convenient and reduce the chance of errors; pull-reqs
improving this process would be much appreciated.

Unless `--clean-merge 0` is specified, `verify-commits.py` will attempt to verify that
each merge commit applies cleanly (with some exceptions). This requires using at least
git v2.38.0.

Configuration files
-------------------

* `trusted-git-root`: This file should contain a single git commit hash which is the first unsigned git commit (hence it is the "root of trust").
* `trusted-sha512-root-commit`: This file should contain a single git commit hash which is the first commit without a SHA512 root commitment.
* `trusted-keys`: This file should contain a \n-delimited list of all PGP fingerprints of authorized commit signers (primary, not subkeys).
* `allow-revsig-commits`: This file should contain a \n-delimited list of git commit hashes. See next section for more info.
* `allow-unclean-merge-commits`: A \n-delimited list of commits exempted from clean-merge verification.
* `allow-incorrect-sha512-commits`: A \n-delimited list of commits exempted from the Tree-SHA512 check.

The exception lists are trust-policy inputs too. Review each exception before
replacing inherited entries with project-owned values; do not copy them into a
new policy without review.

Import trusted keys
-------------------
Do not import the current `trusted-keys` entries as ConnectCoin maintainers;
they are inherited Bitcoin Core trust data. After that file has been replaced
under an approved ConnectCoin policy, the project documentation should provide
an authenticated key-distribution and verification procedure. Merely fetching
fingerprints from a public keyserver is not sufficient to establish trust.


Key expiry/revocation
---------------------

When a key (or subkey) which has signed old commits expires or is revoked,
verify-commits will start failing to verify all commits which were signed by
said key. In order to avoid bumping the root-of-trust `trusted-git-root`
file, individual commits which were signed by such a key can be added to the
`allow-revsig-commits` file. That way, the PGP signatures are still verified
but commits not explicitly listed remain subject to the expiry/revocation check.
The current verifier derives its `allow_revsig` decision from membership in
`allow-revsig-commits`; it does not read a
`CONNECTCOIN_VERIFY_COMMITS_ALLOW_REVSIG` environment variable. Any proposed
exception must be independently reviewed under the project's trust policy,
not automatically accepted merely because verification otherwise fails.
