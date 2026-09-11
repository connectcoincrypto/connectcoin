# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check the actual CI build block without running a build or the CI setup."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which("bash")
SOURCE = (ROOT / "ci/test/03_test_script.sh").read_text(encoding="utf-8")
START = 'if [[ "${GOAL}" != all && "${GOAL}" != codegen ]]; then'
END = 'if [[ "${RUN_IWYU}" == true ]]; then'
BUILD_BLOCK = SOURCE[SOURCE.index(START):SOURCE.index(END, SOURCE.index(START))]


@unittest.skipUnless(BASH, "bash is required")
class BuildInvocationTests(unittest.TestCase):
    def test_build_runs_once_with_verbose_commands_and_original_parallelism(self):
        for goal in ("all", "codegen", "install", "connectcoind connectcoin-qt"):
            for exit_code in (0, 1, 23):
                with self.subTest(goal=goal, exit_code=exit_code):
                    with tempfile.TemporaryDirectory(prefix="connectcoin build check ") as temporary:
                        invocation = Path(temporary) / "invocation"
                        finished = Path(temporary) / "finished"
                        env = dict(os.environ, GOAL=goal, MAKEJOBS="-j4",
                                   BASE_BUILD_DIR="build with spaces", BUILD_EXIT=str(exit_code),
                                   INVOCATION=invocation.as_posix(), FINISHED=finished.as_posix())
                        # The shell function records arguments; it never invokes CMake.
                        script = """set -e -o pipefail
cmake() {
    printf '%s\\0' "$@" >> "$INVOCATION"
    return "$BUILD_EXIT"
}
""" + BUILD_BLOCK + '\nprintf done > "$FINISHED"\n'
                        result = subprocess.run([BASH, "-c", script], env=env,
                                                capture_output=True, text=True, timeout=10, check=False)
                        self.assertEqual(result.returncode, exit_code, result.stderr)
                        targets = ["all"] if goal == "all" else ["codegen"] if goal == "codegen" else ["all", *goal.split()]
                        self.assertEqual(invocation.read_bytes().split(b"\0")[:-1],
                                         [value.encode() for value in
                                          ["--build", "build with spaces", "-j4", "--target", *targets, "--verbose"]])
                        self.assertEqual(finished.exists(), exit_code == 0)


if __name__ == "__main__":
    unittest.main()
