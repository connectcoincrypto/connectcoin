# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Require the descriptor-limit portability mocks in the lint suite."""

import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[2]
    subprocess.run([
        sys.executable, str(root / 'test/util/fs_helpers_mock_tests.py'),
        '--cxx', os.environ.get('CXX', 'c++'),
    ], check=True, timeout=480)


if __name__ == '__main__':
    main()
