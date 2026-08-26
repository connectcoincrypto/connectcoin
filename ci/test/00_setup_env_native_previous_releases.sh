#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

printf '%s\n' \
  'Previous-release CI is disabled until ConnectCoin publishes project-owned release binaries and checksums.' \
  >&2
exit 1
