# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise CI SDK archive cleanup without package installs or network access."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


PLATFORMS = {
    'macos': ('Xcode-1-TEST-extracted-SDK-with-libcxx-headers', ['Xcode-1-TEST-extracted-SDK-with-libcxx-headers.tar'], 'sha256'),
    'netbsd': ('netbsd-test', ['base.tar.xz', 'comp.tar.xz'], 'sha512'),
    'freebsd': ('freebsd-test', ['base-1.txz'], 'sha256'),
    'openbsd': ('openbsd-test', ['base.tgz', 'comp.tgz'], 'sha256'),
}

# Only the SDK subsection runs. All download, verification, extraction, and
# symlink commands are replaced; the sole real removal is one checked fixture
# archive. Bash obtains its own POSIX path, including on Git Bash for Windows.
HARNESS = r'''
set -euo pipefail
DEPENDS_DIR="$(pwd -P)/depends with spaces"
EVENTS="$(pwd -P)/events.log"
PLATFORM=$1
FAIL_STAGE=$3
FAIL_ARCHIVE=$4
if [[ $2 != unset ]]; then CI_IMAGE_BUILD=$2; fi
CI_RETRY_EXE=
XCODE_VERSION= XCODE_BUILD_ID= SDK_URL=https://invalid.example
OSX_SDK_SHA256=digest NETBSD_VERSION= NETBSD_SDK_BASENAME=netbsd-test
NETBSD_SDK_SHA512SUMS='digest base.tar.xz\ndigest comp.tar.xz'
FREEBSD_VERSION= FREEBSD_SDK_BASENAME=freebsd-test FREEBSD_SDK_SHA256=digest
OPENBSD_VERSION= OPENBSD_SDK_BASENAME=openbsd-test
OPENBSD_SDK_SHA256SUMS='digest base.tgz\ndigest comp.tgz'
case $PLATFORM in
  macos) XCODE_VERSION=1; XCODE_BUILD_ID=TEST ;;
  netbsd) NETBSD_VERSION=1 ;;
  freebsd) FREEBSD_VERSION=1 ;;
  openbsd) OPENBSD_VERSION=1 ;;
  *) exit 90 ;;
esac
event() { printf '%s %s\n' "$1" "${2##*/}" >> "$EVENTS"; }
archive_path() {
  [[ ${1%/*} == "$DEPENDS_DIR/sdk-sources" && ${1##*/} != . && ${1##*/} != .. && ! -L $1 ]] || {
    printf 'Refusing archive outside fixture: %s\n' "$1" >&2
    exit 91
  }
}
curl() {
  [[ $# == 5 && $1 == --location && $2 == --fail && $4 == -o ]] || exit 92
  archive_path "$5"
  event download "$5"
  printf 'fixture archive\n' > "$5"
}
checksum() {
  local kind=$1 digest archive
  [[ $# == 2 && $2 == -c ]] || exit 93
  read -r digest archive
  archive_path "$archive"
  [[ $digest == digest && -f $archive ]] || exit 94
  event "$kind" "$archive"
  if [[ $FAIL_STAGE == checksum && ${archive##*/} == "$FAIL_ARCHIVE" ]]; then return 31; fi
}
sha256sum() { checksum sha256 "$@"; }
sha512sum() { checksum sha512 "$@"; }
tar() {
  [[ $# == 4 && $1 == -C && $3 == -xf ]] || exit 95
  local destination=$2 archive=$4
  archive_path "$archive"
  [[ -f $archive && $destination == "$DEPENDS_DIR/SDKs"* ]] || exit 96
  event extract "$archive"
  if [[ $FAIL_STAGE == extract && ${archive##*/} == "$FAIL_ARCHIVE" ]]; then return 32; fi
  if [[ $PLATFORM == macos ]]; then destination+="/$OSX_SDK_BASENAME"; fi
  command mkdir -p "$destination/usr/lib"
  printf 'extracted SDK fixture\n' > "$destination/${archive##*/}.extracted"
}
ln() {
  [[ $# == 3 && $1 == -sf && $(pwd -P) == "$DEPENDS_DIR/SDKs/openbsd-test/usr/lib" ]] || exit 97
  event symlink "$3"
}
rm() {
  [[ $# == 3 && $1 == -f && $2 == -- ]] || exit 98
  archive_path "$3"
  [[ -f $3 ]] || exit 99
  event remove "$3"
  command rm -- "$3"
}
'''


def sdk_section(root):
    source = (root / 'ci/test/01_base_install.sh').read_text(encoding='utf-8')
    start = 'mkdir -p "${DEPENDS_DIR}/SDKs" "${DEPENDS_DIR}/sdk-sources"'
    end = 'echo -n "done" > "${CFG_DONE}"'
    assert source.count(start) == source.count(end) == 1, 'SDK subsection boundaries changed; review the isolated harness'
    assert source.index(start) < source.index(end), 'SDK subsection boundaries are reversed'
    return source[source.index(start):source.index(end)]


def check_image_wiring(root):
    imagefile = (root / 'ci/test_imagefile').read_text(encoding='utf-8')
    for line in imagefile.splitlines():
        if not line.startswith('RUN ['):
            continue
        command = json.loads(line.removeprefix('RUN '))
        if command[:2] != ['bash', '-c'] or len(command) != 3:
            continue
        tokens = shlex.split(command[2])
        if tokens[-1:] == ['./ci/test/01_base_install.sh']:
            assignments = tokens[tokens.index('&&', tokens.index('source')) + 1:-1]
            assert 'CI_IMAGE_BUILD=1' in assignments, 'Docker image installation must enable SDK archive cleanup'
            assert all('=' in token for token in assignments), 'Expected environment assignments immediately before SDK installation'
            return
    raise AssertionError('Could not find the Docker image base-install invocation')


def check_case(*, bash, script, directory, platform, image, failure='', cached=False):
    sdk_name, archives, checksum_kind = PLATFORMS[platform]
    label = f'{platform}, image={image}, failure={failure or "none"}, cached={cached}'
    fixture = directory / label.replace(',', '').replace('=', '-')
    sources = fixture / 'depends with spaces' / 'sdk-sources'
    sources.mkdir(parents=True)
    sentinel = sources / 'unrelated archive.keep'
    sentinel.write_text('preserve unrelated downloads\n', encoding='utf-8')
    sdk_sentinel = fixture / 'depends with spaces' / 'SDKs' / 'unrelated SDK' / 'keep'
    sdk_sentinel.parent.mkdir(parents=True)
    sdk_sentinel.write_text('preserve existing SDK\n', encoding='utf-8')
    if cached:
        for archive in archives:
            (sources / archive).write_text('fixture archive\n', encoding='utf-8')

    # Do not inherit BASH_ENV, exported shell functions, or host CI variables.
    # SystemRoot is needed by Windows process startup; Bash resolves fixture
    # command paths through its own /usr/bin and /bin directories.
    env = {key: os.environ[key] for key in ('SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP') if key in os.environ}
    env.update(PATH='/usr/bin:/bin', LC_ALL='C')
    result = subprocess.run(
        [bash, '--noprofile', '--norc', '-c', HARNESS + '\n' + script, 'sdk-archive-test', platform, image, failure, archives[-1]],
        cwd=fixture, env=env, capture_output=True, text=True, encoding='utf-8', timeout=20,
    )
    event_path = fixture / 'events.log'
    events = event_path.read_text(encoding='utf-8').splitlines() if event_path.exists() else []

    def require(condition, description):
        if not condition:
            raise AssertionError(f'{label}: {description}\nexit={result.returncode}\nevents={events}\nstdout={result.stdout}\nstderr={result.stderr}')

    expected_events = []
    for index, archive in enumerate(archives):
        failing = bool(failure) and index == len(archives) - 1
        if not cached:
            expected_events.append(f'download {archive}')
        expected_events.append(f'{checksum_kind} {archive}')
        if not (failing and failure == 'checksum'):
            expected_events.append(f'extract {archive}')
        if image == '1' and not failing:
            expected_events.append(f'remove {archive}')
        extracted = fixture / 'depends with spaces' / 'SDKs' / sdk_name / f'{archive}.extracted'
        require(extracted.exists() == (not failing), f'incorrect extracted SDK state for {archive}')
        if extracted.exists():
            require(extracted.read_text(encoding='utf-8') == 'extracted SDK fixture\n', f'extracted SDK changed for {archive}')
        require((sources / archive).exists() == (image != '1' or failing), f'incorrect archive retention for {archive}')
        if (sources / archive).exists():
            require((sources / archive).read_text(encoding='utf-8') == 'fixture archive\n', f'archive contents changed for {archive}')
    if platform == 'openbsd' and not failure:
        expected_events.extend(f'symlink {name}' for name in ('libc++abi.so', 'libc++.so', 'libpthread.so'))
    require(result.returncode == {'': 0, 'checksum': 31, 'extract': 32}[failure], 'failure must stop before cleanup with its original status')
    require(events == expected_events, 'download, verification, extraction, and cleanup ordering changed')
    require(sentinel.read_text(encoding='utf-8') == 'preserve unrelated downloads\n', 'unrelated archive changed')
    require(sdk_sentinel.read_text(encoding='utf-8') == 'preserve existing SDK\n', 'unrelated SDK changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bash', default=os.environ.get('BASH', 'bash'), help='Bash executable (defaults to BASH or bash)')
    args = parser.parse_args()
    bash = shutil.which(args.bash)
    if not bash:
        parser.error(f'Bash executable not found: {args.bash}')
    root = Path(__file__).resolve().parents[2]
    script = sdk_section(root)
    check_image_wiring(root)
    count = 0
    with tempfile.TemporaryDirectory(prefix='connectcoin SDK archives ') as temporary:
        directory = Path(temporary)
        for platform in PLATFORMS:
            for image in ('unset', '0', '1'):
                for cached in (False, True):
                    check_case(bash=bash, script=script, directory=directory, platform=platform, image=image, cached=cached)
                    count += 1
                for failure in ('checksum', 'extract'):
                    check_case(bash=bash, script=script, directory=directory, platform=platform, image=image, failure=failure)
                    count += 1
    print(f'CI SDK archives: {count} isolated cases and Docker image wiring passed')


if __name__ == '__main__':
    main()
