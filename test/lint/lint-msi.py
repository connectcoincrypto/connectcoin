# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Run portable MSI validation regressions without installing a package."""

from pathlib import Path
import subprocess
import sys


if __name__ == "__main__":
    subprocess.run([
        sys.executable,
        str(Path(__file__).resolve().parents[2] / "contrib/windeploy/test_msi.py"),
        "--self-test",
    ], check=True)
