#!/usr/bin/env bash
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
export LC_ALL=C
set -euo pipefail

usage() {
    printf '%s\n' \
        'Usage: build-portable.sh --staging-prefix DIR --output-dir DIR --version VERSION' \
        '                         [--format both|tar|appimage] [--qmake PATH]' \
        '' \
        'Run on Linux x86_64 with a Qt 6 build installed in DIR/bin and DIR/share.' \
        'Build on Ubuntu 22.04 to target glibc 2.35 or newer.' \
        'The original staging directory is never changed. Existing outputs are refused.' \
        'Produces a runnable directory and the requested tar.gz and/or AppImage.'
}
die() { printf 'build-portable: %s\n' "$*" >&2; exit 1; }

staging_prefix='' output_dir='' version='' format=both qmake=${QMAKE:-qmake6}
while (($#)); do
    case "$1" in
        --staging-prefix|--output-dir|--version|--format|--qmake)
            (($# >= 2)) || die "Missing value for $1"
            case "$1" in
                --staging-prefix) staging_prefix=$2 ;;
                --output-dir) output_dir=$2 ;;
                --version) version=$2 ;;
                --format) format=$2 ;;
                --qmake) qmake=$2 ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument: $1" ;;
    esac
done
[[ -n "$staging_prefix" && -n "$output_dir" && -n "$version" ]] || { usage >&2; exit 2; }
[[ "$version" =~ ^[0-9][A-Za-z0-9.+_-]*$ ]] || die 'Version must begin with a digit and contain only letters, digits, dots, +, _, or -'
case "$format" in both|tar|appimage) ;; *) die 'Format must be both, tar, or appimage' ;; esac
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'Packaging requires Linux x86_64'
for command in curl sha256sum file readelf tar gzip realpath find sort tail sed cp install mv mktemp desktop-file-validate; do
    command -v "$command" >/dev/null || die "Required command not found: $command"
done
qmake=$(command -v "$qmake") || die 'Qt 6 qmake not found; pass --qmake /path/to/qmake6'
[[ $("$qmake" -query QT_VERSION) == 6.* ]] || die 'The selected qmake must belong to Qt 6'
qt_plugins=$("$qmake" -query QT_INSTALL_PLUGINS)
[[ -f "$qt_plugins/platforms/libqxcb.so" && -f "$qt_plugins/platforms/libqminimal.so" ]] || die 'Install Qt 6 X11 and minimal platform plugins (qt6-qpa-plugins on Ubuntu)'

script_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
source_dir=$(realpath -- "$script_dir/../..")
staging_prefix=$(realpath -e -- "$staging_prefix")
[[ -d "$staging_prefix/bin" && "$staging_prefix" != / && "$staging_prefix" != /usr && "$staging_prefix" != /usr/local ]] || die 'Use a private installation staging prefix, not a system prefix'
output_dir=$(realpath -m -- "$output_dir")
[[ "$output_dir" != / && "$output_dir" != "$source_dir" && "$output_dir" != "${HOME:-/}" ]] || die 'Choose a dedicated output subdirectory'
case "$output_dir/" in "$staging_prefix/"*) die 'Output directory must not be inside the staging prefix' ;; esac
case "$staging_prefix/" in "$output_dir/"*) die 'Staging prefix must not be inside the output directory' ;; esac

binaries=(connectcoin connectcoin-qt connectcoind connectcoin-cli connectcoin-wallet connectcoin-tx connectcoin-util)
for binary in "${binaries[@]}"; do
    [[ -f "$staging_prefix/bin/$binary" && -x "$staging_prefix/bin/$binary" ]] || die "Missing installed executable: bin/$binary"
    file -Lb -- "$staging_prefix/bin/$binary" | grep -q 'ELF 64-bit.*x86-64' || die "Expected Linux x86_64 ELF executable: $binary"
done
# Do not copy symlinks which point outside the staging installation.
while IFS= read -r -d '' link; do
    resolved=$(realpath -e -- "$link") || die "Broken staged symlink: $link"
    case "$resolved" in "$staging_prefix/"*) ;; *) die "Staged symlink escapes the installation: $link" ;; esac
done < <(find "$staging_prefix" -type l -print0)

bundle_name="connectcoin-${version}-linux-x86_64"
appimage_name="ConnectCoin-Core-${version}-x86_64.AppImage"
mkdir -p -- "$output_dir"
for target in "$bundle_name" "$bundle_name.tar.gz" "$appimage_name"; do
    [[ ! -e "$output_dir/$target" && ! -L "$output_dir/$target" ]] || die "Output already exists: $output_dir/$target"
done
work_dir=$(mktemp -d "$output_dir/.portable-build.XXXXXXXX")
cleanup() {
    local status=$?
    if ((status == 0)); then
        # Only remove the private directory returned by mktemp above.
        case "$work_dir" in "$output_dir"/.portable-build.*) rm -rf -- "$work_dir" ;; esac
    else
        printf 'Packaging failed; diagnostic staging retained at %s\n' "$work_dir" >&2
    fi
}
trap cleanup EXIT
tools_dir="$work_dir/tools"
appdir="$work_dir/$bundle_name"
mkdir -p -- "$tools_dir" "$appdir/usr"
cp -a -- "$staging_prefix/." "$appdir/usr/"

# Digests are the official GitHub release asset SHA256 metadata, checked 2026-10-01.
# The Qt plugin's continuous release is pinned by asset ID AND hash: an upstream
# replacement cannot silently change this build. See README.md for provenance.
linuxdeploy_url='https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage'
linuxdeploy_sha256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
qt_plugin_url='https://api.github.com/repos/linuxdeploy/linuxdeploy-plugin-qt/releases/assets/525032210'
qt_plugin_sha256=cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
runtime_url='https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64'
runtime_sha256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
download_verified() {
    local url=$1 expected=$2 destination=$3
    curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
        --header 'Accept: application/octet-stream' --output "$destination" "$url"
    printf '%s  %s\n' "$expected" "$destination" | sha256sum --check --strict -
    chmod 755 -- "$destination"
}
download_verified "$linuxdeploy_url" "$linuxdeploy_sha256" "$tools_dir/linuxdeploy-x86_64.AppImage"
download_verified "$qt_plugin_url" "$qt_plugin_sha256" "$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage"

desktop_file="$appdir/usr/share/applications/org.connectcoin.ConnectCoin.desktop"
icon_file="$appdir/usr/share/icons/hicolor/256x256/apps/connectcoin.png"
install -D -m 644 "$source_dir/share/applications/org.connectcoin.ConnectCoin.desktop" "$desktop_file"
install -D -m 644 "$source_dir/share/pixmaps/connectcoin256.png" "$icon_file"
if [[ ! -f "$appdir/usr/share/pixmaps/connectcoin.png" ]]; then
    install -D -m 644 "$source_dir/src/qt/res/icons/connectcoin.png" "$appdir/usr/share/pixmaps/connectcoin.png"
fi
install -D -m 644 "$source_dir/COPYING" "$appdir/usr/share/doc/connectcoin/COPYING"
install -m 644 "$script_dir/PORTABLE-README.txt" "$appdir/README.txt"
install -m 644 "$script_dir/AppImage-runtime-LICENSE.txt" "$appdir/usr/share/doc/connectcoin/AppImage-runtime-LICENSE.txt"
desktop-file-validate "$desktop_file"

export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 QMAKE="$qmake"
export PATH="$tools_dir:$PATH"
# linuxdeploy copies distribution copyright files alongside deployed libraries.
unset DISABLE_COPYRIGHT_FILES_DEPLOYMENT
export EXTRA_PLATFORM_PLUGINS=libqminimal.so
for platform in libqoffscreen.so libqwayland-egl.so libqwayland-generic.so; do
    if [[ -f "$qt_plugins/platforms/$platform" ]]; then
        EXTRA_PLATFORM_PLUGINS+=";$platform"
    fi
done
deploy_args=(--appdir "$appdir" --desktop-file "$desktop_file" --icon-file "$icon_file" --custom-apprun "$script_dir/apprun")
for binary in "${binaries[@]}"; do
    deploy_args+=(--executable "$appdir/usr/bin/$binary")
done
"$tools_dir/linuxdeploy-x86_64.AppImage" "${deploy_args[@]}" --plugin qt

[[ -f "$appdir/usr/plugins/platforms/libqxcb.so" && -f "$appdir/usr/plugins/platforms/libqminimal.so" ]] || die 'Qt platform plugin deployment is incomplete'
[[ -f "$appdir/usr/lib/libQt6Core.so.6" && -f "$appdir/usr/bin/qt.conf" ]] || die 'Qt 6 library or relative qt.conf was not deployed'
install -m 755 "$script_dir/apprun" "$appdir/AppRun"
for binary in "${binaries[@]}"; do
    [[ ! -e "$appdir/$binary" ]] || die "Unexpected file at bundle root: $binary"
    ln -s AppRun "$appdir/$binary"
done
# Absolute host symlinks must not leak into a portable artifact.
while IFS= read -r -d '' link; do
    [[ $(readlink -- "$link") != /* ]] || die "Absolute symlink in bundle: $link"
    resolved=$(realpath -e -- "$link") || die "Broken bundled symlink: $link"
    case "$resolved" in "$appdir/"*) ;; *) die "Bundled symlink escapes AppDir: $link" ;; esac
done < <(find "$appdir" -type l -print0)

# A newer build host must not silently raise the advertised glibc baseline.
required_glibc=$(
    while IFS= read -r -d '' candidate; do
        readelf --version-info "$candidate" 2>/dev/null || true
    done < <(find "$appdir/usr" -type f -print0) |
        sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1
)
[[ -n "$required_glibc" ]] || die 'Could not determine the glibc requirement of the bundle'
[[ $(printf '%s\n' 2.35 "$required_glibc" | sort -V | tail -n 1) == 2.35 ]] || die "Bundle requires glibc $required_glibc; rebuild on Ubuntu 22.04 (maximum 2.35)"
{
    printf 'ConnectCoin Core %s\nArchitecture: x86_64\nRequired GLIBC symbols: %s\nQt: %s\n' "$version" "$required_glibc" "$("$qmake" -query QT_VERSION)"
    printf 'linuxdeploy SHA256: %s\nQt plugin SHA256: %s\nAppImage runtime SHA256: %s\n' "$linuxdeploy_sha256" "$qt_plugin_sha256" "$runtime_sha256"
} > "$appdir/PACKAGING.txt"

if [[ "$format" != tar ]]; then
    download_verified "$runtime_url" "$runtime_sha256" "$tools_dir/runtime-x86_64"
    export LDAI_RUNTIME_FILE="$tools_dir/runtime-x86_64" LDAI_OUTPUT="$work_dir/$appimage_name"
    export LDAI_VERSION="$version" LDAI_NO_APPSTREAM=1
    "$tools_dir/linuxdeploy-x86_64.AppImage" --appdir "$appdir" --custom-apprun "$script_dir/apprun" --output appimage
    [[ -s "$work_dir/$appimage_name" ]] || die 'AppImage output was not created'
    chmod 755 -- "$work_dir/$appimage_name"
fi
if [[ "$format" != appimage ]]; then
    tar --sort=name --owner=0 --group=0 --numeric-owner -C "$work_dir" -czf "$work_dir/$bundle_name.tar.gz" "$bundle_name"
fi
mv -- "$appdir" "$output_dir/$bundle_name"
for artifact in "$work_dir/$bundle_name.tar.gz" "$work_dir/$appimage_name"; do
    if [[ -f "$artifact" ]]; then
        mv -- "$artifact" "$output_dir/"
    fi
done
printf 'Portable artifacts and runnable directory written to %s\n' "$output_dir"
