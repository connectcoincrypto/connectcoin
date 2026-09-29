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
START = 'BUILD_GOALS="${GOAL}"'
END = 'if [[ "${RUN_IWYU}" == true ]]; then'
BUILD_BLOCK = SOURCE[SOURCE.index(START):SOURCE.index(END, SOURCE.index(START))]
DEPLOY_START = 'if [[ "$CI_OS_NAME" == "macos" && "${GOAL}" = "install deploy" ]]; then'
DEPLOY_END = 'if [ "$RUN_UNIT_TESTS" = "true" ]; then'
DEPLOY_BLOCK = SOURCE[SOURCE.index(DEPLOY_START):SOURCE.index(DEPLOY_END)]


@unittest.skipUnless(BASH, "bash is required")
class BuildInvocationTests(unittest.TestCase):
    def test_build_runs_once_with_verbose_commands_and_original_parallelism(self):
        for goal in ("all", "codegen", "install", "install deploy", "connectcoind connectcoin-qt"):
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

    def test_deploy_verification_after_build(self):
        for os_name, goal, unzip_exit, codesign_exit in (
                ("macos", "install deploy", 0, 0),
                ("macos", "install deploy", 7, 0),
                ("macos", "install deploy", 0, 9),
                ("macos", "install", 0, 0),
                ("linux", "install deploy", 0, 0)):
            with self.subTest(os_name=os_name, goal=goal, unzip_exit=unzip_exit,
                              codesign_exit=codesign_exit):
                with tempfile.TemporaryDirectory(prefix="connectcoin deploy check ") as temporary:
                    invocation = Path(temporary) / "invocation"
                    env = dict(os.environ, GOAL=goal, MAKEJOBS="-j4", CI_OS_NAME=os_name,
                               BASE_BUILD_DIR="build with spaces", UNZIP_EXIT=str(unzip_exit),
                               CODESIGN_EXIT=str(codesign_exit), INVOCATION=invocation.as_posix())
                    script = """set -e -o pipefail
cmake() { return 0; }
unzip() {
    printf '%s\\0' unzip "$@" >> "$INVOCATION"
    return "$UNZIP_EXIT"
}
codesign() {
    printf '%s\\0' codesign "$@" >> "$INVOCATION"
    return "$CODESIGN_EXIT"
}
""" + BUILD_BLOCK + DEPLOY_BLOCK
                    result = subprocess.run([BASH, "-c", script], env=env,
                                            capture_output=True, text=True, timeout=10, check=False)
                    if os_name != "macos" or goal != "install deploy":
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertFalse(invocation.exists())
                        continue
                    expected = ["unzip", "build with spaces/connectcoin-macos-app.zip",
                                "-d", "build with spaces/deploy"]
                    if unzip_exit == 0:
                        expected.extend(["codesign", "--verify",
                                         "build with spaces/deploy/ConnectCoin-Qt.app"])
                    self.assertEqual(invocation.read_bytes().split(b"\0")[:-1],
                                     [value.encode() for value in expected])
                    self.assertEqual(result.returncode, unzip_exit or (1 if codesign_exit else 0),
                                     result.stderr)


if __name__ == "__main__":
    unittest.main()
