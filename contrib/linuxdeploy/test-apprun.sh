#!/usr/bin/env bash
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
# The generated fake tools and argument-preservation tests need literal dollars.
# shellcheck disable=SC2016
# Test launcher behavior without running Core, Qt, or any downloaded tools.
set -euo pipefail
script_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
test_dir=$(mktemp -d)
cleanup() {
    [[ -n "$test_dir" && -d "$test_dir" ]] && rm -rf -- "$test_dir"
}
trap cleanup EXIT
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

bundle="$test_dir/original bundle"
mkdir -p "$bundle/usr/bin" "$bundle/usr/lib" "$bundle/usr/plugins/platforms"
cp "$script_dir/apprun.sh" "$bundle/AppRun"
# Copies exercise argv[0] dispatch even on Windows filesystems without symlinks.
cp "$script_dir/apprun.sh" "$bundle/connectcoin-cli"
for tool in connectcoin-qt connectcoin-cli connectcoind; do
    printf '%s\n' \
        '#!/bin/sh' \
        'printf "tool=%s\n" "${0##*/}"' \
        'printf "arg=<%s>\n" "$@"' \
        'printf "lib=%s\nqt=%s\nqpa=%s\n" "$LD_LIBRARY_PATH" "$QT_PLUGIN_PATH" "$QT_QPA_PLATFORM_PLUGIN_PATH"' \
        'exit "${TEST_TOOL_EXIT:-0}"' > "$bundle/usr/bin/$tool"
    chmod +x "$bundle/usr/bin/$tool"
done
chmod +x "$bundle/AppRun" "$bundle/connectcoin-cli"

mv "$bundle" "$test_dir/moved bundle with spaces"
bundle="$test_dir/moved bundle with spaces"
result=$(LD_LIBRARY_PATH=/example/inherited "$bundle/AppRun" 'argument with spaces' 'literal;$HOME')
[[ "$result" == *'tool=connectcoin-qt'* ]] || fail 'Default invocation did not select GUI'
[[ "$result" == *'arg=<argument with spaces>'* && "$result" == *'arg=<literal;$HOME>'* ]] || fail 'Arguments were altered'
[[ "$result" == *"lib=$bundle/usr/lib:/example/inherited"* ]] || fail 'Library path does not follow relocated bundle'
[[ "$result" == *"qt=$bundle/usr/plugins"* && "$result" == *"qpa=$bundle/usr/plugins/platforms"* ]] || fail 'Qt plugin paths do not follow relocated bundle'

result=$("$bundle/connectcoin-cli" -help)
[[ "$result" == *'tool=connectcoin-cli'* ]] || fail 'Named launcher did not select CLI'
result=$("$bundle/AppRun" --tool connectcoind -help)
[[ "$result" == *'tool=connectcoind'* && "$result" == *'arg=<-help>'* ]] || fail 'Explicit tool dispatch failed'

set +e
"$bundle/AppRun" --tool ../outside > /dev/null 2>&1
invalid_status=$?
"$bundle/AppRun" --tool > /dev/null 2>&1
missing_status=$?
TEST_TOOL_EXIT=17 "$bundle/AppRun" -help > /dev/null 2>&1
exit_status=$?
set -e
[[ $invalid_status == 2 && $missing_status == 2 ]] || fail 'Invalid tool arguments were not rejected'
[[ $exit_status == 17 ]] || fail 'Executable exit status was not preserved'
printf '%s\n' 'AppRun relocation, dispatch, argument, environment, and exit-status checks passed.'
