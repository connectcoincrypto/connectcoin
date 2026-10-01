#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Record release provenance without including paths or environment secrets."""

import argparse
import hashlib
import json
import platform
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    files = []
    for path in sorted(args.artifacts.iterdir()):
        if path.is_file() and path.name.endswith(('.deb', '.rpm', '.AppImage', '.tar.gz')):
            digest = hashlib.sha256()
            with path.open('rb') as package:
                for chunk in iter(lambda: package.read(1024 * 1024), b''):
                    digest.update(chunk)
            files.append({'name': path.name, 'bytes': path.stat().st_size,
                          'sha256': digest.hexdigest()})
    if not files:
        parser.error('no release packages found')
    cache = {}
    allowed = {'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'ENABLE_IPC', 'ENABLE_WALLET',
               'WITH_ZMQ', 'BUILD_GUI', 'CONNECTCOIN_PACKAGE_RELEASE'}
    for line in (args.build_dir / 'CMakeCache.txt').read_text(encoding='utf8').splitlines():
        if '=' in line and ':' in line and line.split(':', 1)[0] in allowed:
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no']))
    if dirty:
        parser.error('tracked sources changed during the release build')
    result = {'commit': commit, 'architecture': platform.machine(),
              'os_release': Path('/etc/os-release').read_text(encoding='utf8'),
              'configuration': cache, 'files': files,
              'release_published': False}
    (args.artifacts / 'build-info.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')
    (args.artifacts / 'SHA256SUMS').write_text(
        ''.join(f"{entry['sha256']}  {entry['name']}\n" for entry in files), encoding='utf8')


if __name__ == '__main__':
    main()
