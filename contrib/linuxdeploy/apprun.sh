#!/bin/sh
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
set -eu

# AppImage and the extracted tarball use the same relative directory layout.
bundle_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
tool=${0##*/}
case "$tool" in
    connectcoin|connectcoind|connectcoin-qt|connectcoin-cli|connectcoin-wallet|connectcoin-tx|connectcoin-util) ;;
    *) tool=connectcoin-qt ;;
esac
if [ "${1-}" = --tool ]; then
    [ "$#" -ge 2 ] || { printf '%s\n' 'AppRun: --tool requires an executable name' >&2; exit 2; }
    tool=$2
    shift 2
fi
case "$tool" in
    connectcoin|connectcoind|connectcoin-qt|connectcoin-cli|connectcoin-wallet|connectcoin-tx|connectcoin-util) ;;
    *) printf 'AppRun: unsupported tool: %s\n' "$tool" >&2; exit 2 ;;
esac

export PATH="$bundle_dir/usr/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="$bundle_dir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$bundle_dir/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$bundle_dir/usr/plugins/platforms"
export XDG_DATA_DIRS="$bundle_dir/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$bundle_dir/usr/bin/$tool" "$@"
