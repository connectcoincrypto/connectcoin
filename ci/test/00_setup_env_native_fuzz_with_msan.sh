#!/usr/bin/env bash
#
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:26.04"
export APT_LLVM_V="22"
LIBCXX_DIR="/cxx_build/"
export MSAN_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -g -O1 -fno-optimize-sibling-calls"
LIBCXX_FLAGS="-nostdinc++ -nostdlib++ -isystem ${LIBCXX_DIR}include/c++/v1 -L${LIBCXX_DIR}lib -Wl,-rpath,${LIBCXX_DIR}lib -lc++ -lc++abi -lpthread -Wno-unused-command-line-argument"
export MSAN_AND_LIBCXX_FLAGS="${MSAN_FLAGS} ${LIBCXX_FLAGS}"

export FUZZ_SHARD_COUNT="${FUZZ_SHARD_COUNT:-1}"
export FUZZ_SHARD_INDEX="${FUZZ_SHARD_INDEX:-0}"
export CONTAINER_NAME="ci_native_fuzz_msan_${FUZZ_SHARD_INDEX}"
export CI_CACHE_NAME="ci_native_fuzz_msan"
# Split large corpora across the same two workers already allowed below. This
# avoids a single target such as txorphan becoming a two-hour serial tail.
export FUZZ_TESTS_CONFIG="--shard-count=${FUZZ_SHARD_COUNT} --shard-index=${FUZZ_SHARD_INDEX} --corpus-shards=2 --corpus-shard-min-files=512"
export PACKAGES="clang-${APT_LLVM_V} llvm-${APT_LLVM_V} llvm-${APT_LLVM_V}-dev libclang-${APT_LLVM_V}-dev libclang-rt-${APT_LLVM_V}-dev"
export DEP_OPTS="DEBUG=1 NO_QT=1 CC=clang CXX=clang++ CFLAGS='${MSAN_FLAGS}' CXXFLAGS='${MSAN_AND_LIBCXX_FLAGS}'"
export GOAL="all"
# Setting CMAKE_{C,CXX}_FLAGS_DEBUG flags to an empty string ensures that the flags set in MSAN_FLAGS remain unaltered.
# _FORTIFY_SOURCE is not compatible with MSAN.
export CONNECTCOIN_CONFIG="\
 -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_C_FLAGS_DEBUG='' \
 -DCMAKE_CXX_FLAGS_DEBUG='' \
 -DBUILD_FOR_FUZZING=ON \
 -DSANITIZERS=memory \
 -DAPPEND_CPPFLAGS='-DBOOST_MULTI_INDEX_ENABLE_SAFE_MODE -U_FORTIFY_SOURCE' \
"
export USE_INSTRUMENTED_LIBCPP="MemoryWithOrigins"
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
export MAKEJOBS="-j2"  # Keep concurrent instrumented fuzz processes within the runner memory limit.
