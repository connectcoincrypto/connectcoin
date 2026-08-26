#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -o errexit -o pipefail -o xtrace

# Fixes permission issues when there is a container UID/GID mismatch with the owner
# of the mounted ConnectCoin source directory.
git config --global --add safe.directory /connectcoin

export PATH="/python_env/bin:${PATH}"

if [ -n "${LINT_CI_IS_PR}" ]; then
  export COMMIT_RANGE="HEAD~..HEAD"
  if [ "$(git rev-list -1 HEAD)" != "$(git rev-list -1 --merges HEAD)" ]; then
    echo "Error: The top commit must be a merge commit, usually the remote 'pull/<PR_NUMBER>/merge' branch."
    false
  fi
fi

RUST_BACKTRACE=1 cargo run --manifest-path "./test/lint/test_runner/Cargo.toml" -- "$@"
