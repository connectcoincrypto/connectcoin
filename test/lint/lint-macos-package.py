#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Run cross-platform macOS packaging, validation and source-bundle regressions."""

from pathlib import Path
import subprocess
import sys


if __name__ == "__main__":
    helpers = Path(__file__).resolve().parents[2] / "contrib/macdeploy"
    for name in ("build_dmg.py", "validate_dmg.py", "collect_sources.py"):
        subprocess.run([sys.executable, str(helpers / name), "--self-test"], check=True)
