# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Fail closed on oversized compiler-cache exports without failing the CI job."""

import os
from pathlib import Path
import re


def export_limit_bytes(value):
    """Support explicit ccache decimal/binary size units; reject ambiguity."""
    match = re.fullmatch(r'([1-9][0-9]*)([KMGT])(i?)(?:B)?', value, re.IGNORECASE)
    if not match:
        raise ValueError('An explicit positive cache size is required')
    number, unit, binary = match.groups()
    return int(number) * (1024 if binary else 1000) ** ('KMGT'.index(unit.upper()) + 1)


def cache_is_bounded(path, limit):
    """Count all files, including metadata, without traversing cache symlinks."""
    root = Path(path)
    if root.is_symlink() or not root.is_dir():
        return False
    size = 0
    directories = [root]
    while directories:
        with os.scandir(directories.pop()) as entries:
            for entry in entries:
                if entry.is_symlink():
                    return False
                if entry.is_dir(follow_symlinks=False):
                    directories.append(entry.path)
                elif entry.is_file(follow_symlinks=False):
                    status = entry.stat(follow_symlinks=False)
                    # Account for filesystem allocation and sparse file size.
                    size += max(status.st_size, getattr(status, 'st_blocks', 0) * 512)
                    if size > limit:
                        return False
                else:
                    return False
    return True


def main():
    ready = False
    try:
        path = os.environ.get('CCACHE_DIR')
        if path:
            ready = cache_is_bounded(path, export_limit_bytes(os.environ.get('CCACHE_MAXSIZE', '')))
    except (OSError, ValueError):
        pass
    if not ready:
        print('::warning::Skipping compiler-cache export: missing, unreadable, unsafe, or above the export budget.')
    try:
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
            output.write(f'ready={str(ready).lower()}\n')
    except (KeyError, OSError):
        # A missing output leaves the upload gate closed. Cache maintenance
        # must not change the result of compilation or tests.
        print('::warning::Unable to report compiler-cache export readiness; skipping the upload.')


if __name__ == '__main__':
    main()
