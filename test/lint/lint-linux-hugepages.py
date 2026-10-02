#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Run the Linux Huge Pages helper's offline, unprivileged regression tests."""

from pathlib import Path
import subprocess
import sys


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[2]
    sys.exit(subprocess.call([sys.executable, '-m', 'unittest', 'discover', '-s',
                              str(root / 'contrib/linuxdeploy'), '-p', 'test_hugepages.py', '-v'], cwd=root))
