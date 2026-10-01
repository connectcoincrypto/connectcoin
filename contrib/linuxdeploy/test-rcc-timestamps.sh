#!/usr/bin/env bash
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
export LC_ALL=C
set -euo pipefail

# An explicit path also allows testing with the native Windows Qt installation.
if (($# > 1)); then
    printf '%s\n' 'Usage: test-rcc-timestamps.sh [path/to/Qt6/rcc]' >&2
    exit 2
fi
rcc=${1:-}
if [[ -z "$rcc" ]]; then
    for candidate in /usr/lib/qt6/libexec/rcc /usr/lib64/qt6/libexec/rcc rcc-qt6 rcc6; do
        if rcc=$(command -v "$candidate"); then
            break
        fi
    done
fi
[[ -n "$rcc" && -x "$rcc" ]] || { printf '%s\n' 'Qt 6 rcc was not found' >&2; exit 1; }
[[ $("$rcc" --version 2>&1) == 'rcc 6.'* ]] || { printf '%s\n' 'Test requires Qt 6 rcc' >&2; exit 1; }

test_parent=$(cd -- "${TMPDIR:-/tmp}" && pwd -P)
test_dir=$(mktemp -d "$test_parent/connectcoin-rcc-timestamps.XXXXXXXX")
test_dir=$(cd -- "$test_dir" && pwd -P)
cleanup() {
    local resolved
    [[ -d "$test_dir" && ! -L "$test_dir" ]] || return 0
    resolved=$(cd -- "$test_dir" && pwd -P) || return 0
    if [[ "$resolved" == "$test_dir" && "${resolved%/*}" == "$test_parent" &&
          "${resolved##*/}" =~ ^connectcoin-rcc-timestamps\.[a-zA-Z0-9]{8}$ ]]; then
        rm -rf -- "$resolved"
    else
        printf 'Refusing to remove unexpected test directory: %s\n' "$resolved" >&2
    fi
}
trap cleanup EXIT
printf '%s\n' '<RCC><qresource prefix="/probe"><file>payload.txt</file></qresource></RCC>' > "$test_dir/probe.qrc"
printf '%s\n' 'unchanged translation payload' > "$test_dir/payload.txt"
generate() {
    env -u SOURCE_DATE_EPOCH -u QT_RCC_SOURCE_DATE_OVERRIDE \
        "$rcc" --name timestamp_probe "$test_dir/probe.qrc" -o "$test_dir/raw-$1.cpp"
    env -u SOURCE_DATE_EPOCH QT_RCC_SOURCE_DATE_OVERRIDE=1 \
        "$rcc" --name timestamp_probe "$test_dir/probe.qrc" -o "$test_dir/fixed-$1.cpp"
}

touch -t 202001020304.05 "$test_dir/payload.txt"
generate before
touch -t 202101020304.05 "$test_dir/payload.txt"
generate after
if cmp -s "$test_dir/raw-before.cpp" "$test_dir/raw-after.cpp"; then
    printf '%s\n' 'FAIL: control RCC sources did not reflect changed input mtimes' >&2
    exit 1
fi
cmp "$test_dir/fixed-before.cpp" "$test_dir/fixed-after.cpp"

# Normalizing metadata must still allow a real payload change to invalidate the cache.
printf '%s\n' 'changed translation payload' > "$test_dir/payload.txt"
generate changed
if cmp -s "$test_dir/fixed-before.cpp" "$test_dir/fixed-changed.cpp"; then
    printf '%s\n' 'FAIL: RCC sources did not reflect changed payload content' >&2
    exit 1
fi
printf '%s\n' 'Qt RCC timestamp normalization preserves content changes and removes mtime-only changes.'
