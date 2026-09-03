#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:26.04"
export FUZZ_SHARD_COUNT="${FUZZ_SHARD_COUNT:-1}"
export FUZZ_SHARD_INDEX="${FUZZ_SHARD_INDEX:-0}"
export CONTAINER_NAME="ci_native_fuzz_${FUZZ_SHARD_INDEX}"
# All shards compile the same configuration. Share their Docker and compiler
# cache namespace while retaining distinct container names for execution.
export CI_CACHE_NAME="ci_native_fuzz"
export FUZZ_TESTS_CONFIG="--shard-count=${FUZZ_SHARD_COUNT} --shard-index=${FUZZ_SHARD_INDEX} --corpus-shards=4 --corpus-shard-min-files=512"
export APT_LLVM_V="22"
export PACKAGES="clang-${APT_LLVM_V} llvm-${APT_LLVM_V} libclang-rt-${APT_LLVM_V}-dev libboost-dev libsqlite3-dev libcapnp-dev capnproto"
export NO_DEPENDS=1
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
export GOAL="all"
export CI_CONTAINER_CAP="--cap-add SYS_PTRACE"  # If run with (ASan + LSan), the container needs access to ptrace (https://github.com/google/sanitizers/issues/764)
export CONNECTCOIN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON \
 -DSANITIZERS=fuzzer,address,undefined,float-divide-by-zero,integer \
 -DCMAKE_C_COMPILER=clang \
 -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_FLAGS='-ftrivial-auto-var-init=pattern' \
 -DCMAKE_CXX_FLAGS='-ftrivial-auto-var-init=pattern' \
"
