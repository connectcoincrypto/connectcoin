#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -o errexit
# Invalidate completion before setup or Docker can fail. This file is outside
# all restored cache paths and must describe only the current invocation.
if [ -n "${BASE_BUILD_DIR:-}" ]; then
  rm -f -- "${BASE_BUILD_DIR}/.ci-depends-complete"
fi
export CI_DEPENDS_CACHE_RUN="${GITHUB_RUN_ID:-local}:${GITHUB_RUN_ATTEMPT:-0}:${GITHUB_JOB:-local}:${CONTAINER_NAME:-local}"
source ./ci/test/00_setup_env.sh
"./ci/test/02_run_container.py"
