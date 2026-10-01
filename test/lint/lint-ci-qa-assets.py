#!/usr/bin/env python3
# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Run offline QA assets acquisition regression tests."""

from pathlib import Path
import subprocess
import sys


if __name__ == "__main__":
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / "ci/test/test_qa_assets.py")], check=True)
