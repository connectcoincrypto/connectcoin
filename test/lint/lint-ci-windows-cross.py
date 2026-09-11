# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Hermetic contracts for per-CRT build/test scheduling and coverage.

Inspect the checked-in workflow structure without network access or a YAML
dependency. These focused helpers require the repository's explicit indentation;
they are not a general YAML parser. Actionlint checks full workflow syntax.
"""

import itertools
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CALLER = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
REUSABLE = (ROOT / ".github/workflows/ci-windows-cross.yml").read_text(encoding="utf-8")
CRTS = {
    "msvcrt": ("./ci/test/00_setup_env_win64_msvcrt.sh", "x86_64-w64-mingw32-executables"),
    "ucrt": ("./ci/test/00_setup_env_win64.sh", "x86_64-w64-mingw32ucrt-executables"),
}


def section(source, key, indent):
    """Return one explicit mapping's body, rejecting missing/duplicate keys."""
    lines = source.splitlines()
    starts = [index for index, line in enumerate(lines) if line == " " * indent + key + ":"]
    assert len(starts) == 1, (key, starts)
    start = starts[0] + 1
    end = start
    while end < len(lines):
        line = lines[end]
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        end += 1
    return "\n".join(lines[start:end])


def value(source, key, indent):
    matches = re.findall(r"^" + " " * indent + re.escape(key) + r": (.+)$", source, re.M)
    assert len(matches) == 1, (key, matches)
    return matches[0]


def job(source, name):
    return section(section(source, "jobs", 0), name, 2)


def dependency_graph(caller, reusable):
    """Expand the checked-in caller matrix into independent local job graphs."""
    call = job(caller, "windows-cross")
    assert value(call, "needs", 4) == "record-frozen-commit"
    assert value(call, "uses", 4) == "./.github/workflows/ci-windows-cross.yml"
    assert not re.search(r"^    (runs-on|steps):", call, re.M)
    strategy = section(call, "strategy", 4)
    assert value(strategy, "fail-fast", 6) == "false"
    assert value(section(strategy, "matrix", 6), "crt", 8) == "[msvcrt, ucrt]"
    assert not re.search(r"^  windows-native-test:", caller, re.M)
    assert re.findall(r"^  ([\w-]+):", section(reusable, "jobs", 0), re.M) == ["windows-cross", "windows-native-test"]
    build = job(reusable, "windows-cross")
    native = job(reusable, "windows-native-test")
    assert not re.search(r"^    (needs|strategy|if):", build, re.M)
    assert value(native, "needs", 4) == "windows-cross"
    assert not re.search(r"^    (strategy|if):", native, re.M)
    assert "continue-on-error:" not in reusable
    assert "concurrency:" not in reusable  # Do not collide with/cancel the caller.
    return {(crt, "windows-native-test"): {(crt, "windows-cross")} for crt in CRTS}


class WindowsCrossWorkflowTests(unittest.TestCase):
    def test_each_crt_can_test_without_waiting_for_other_build(self):
        graph = dependency_graph(CALLER, REUSABLE)
        for first, other in (("msvcrt", "ucrt"), ("ucrt", "msvcrt")):
            for other_status in ("queued", "running", "failure", "cancelled"):
                with self.subTest(first=first, other_status=other_status):
                    status = {(first, "windows-cross"): "success", (other, "windows-cross"): other_status}
                    ready = {target for target, needs in graph.items() if all(status[dependency] == "success" for dependency in needs)}
                    self.assertEqual(ready, {(first, "windows-native-test")})

    def test_failed_build_cannot_test_or_block_the_other_crt(self):
        graph = dependency_graph(CALLER, REUSABLE)
        for statuses in itertools.product(("success", "failure", "cancelled", "skipped"), repeat=2):
            status = dict(zip(((crt, "windows-cross") for crt in CRTS), statuses))
            for crt in CRTS:
                ready = all(status[dependency] == "success" for dependency in graph[(crt, "windows-native-test")])
                self.assertEqual(ready, status[(crt, "windows-cross")] == "success")

    def test_negative_controls_reject_matrix_barriers_and_failure_masking(self):
        mutations = [
            (CALLER.replace("fail-fast: false", "fail-fast: true"), REUSABLE),
            (CALLER, REUSABLE.replace("needs: windows-cross", "needs: [windows-cross, record-frozen-commit]")),
            (CALLER, REUSABLE.replace("needs: windows-cross", "needs: windows-cross\n    if: always()")),
            (CALLER, REUSABLE.replace("needs: windows-cross", "needs: windows-cross\n    strategy:\n      matrix:\n        crt: [msvcrt, ucrt]")),
            (CALLER, REUSABLE + "\nconcurrency: shared-with-caller\n"),
            (CALLER, REUSABLE.replace("needs: windows-cross", "needs: windows-cross\n    continue-on-error: true")),
        ]
        for caller, reusable in mutations:
            with self.subTest(mutation=(caller, reusable)):
                with self.assertRaises(AssertionError):
                    dependency_graph(caller, reusable)

    def test_fixed_matrix_inputs_and_frozen_checkout(self):
        call = job(CALLER, "windows-cross")
        includes = section(section(section(call, "strategy", 4), "matrix", 6), "include", 8)
        expected = "\n".join(f"          - crt: {crt}\n            file-env: '{file_env}'\n            artifact-name: '{artifact}'"
                             for crt, (file_env, artifact) in CRTS.items())
        self.assertEqual(includes.strip(), expected.strip())
        arguments = section(call, "with", 4)
        self.assertEqual(value(arguments, "commit", 6), "${{ needs.record-frozen-commit.outputs.commit }}")
        for name in ("crt", "file-env", "artifact-name"):
            self.assertEqual(value(arguments, name, 6), "${{ matrix." + name + " }}")
        inputs = section(section(section(REUSABLE, "on", 0), "workflow_call", 2), "inputs", 4)
        self.assertEqual(re.findall(r"^      ([\w-]+):", inputs, re.M), ["commit", "crt", "file-env", "artifact-name"])
        for name in ("commit", "crt", "file-env", "artifact-name"):
            item = section(inputs, name, 6)
            self.assertEqual(value(item, "required", 8), "true")
            self.assertEqual(value(item, "type", 8), "string")
        build = job(REUSABLE, "windows-cross")
        native = job(REUSABLE, "windows-native-test")
        self.assertIn("- &FROZEN_CHECKOUT\n        name: Checkout\n        uses: actions/checkout@v6\n        with:\n          ref: ${{ inputs.commit }}", build)
        self.assertIn("- *FROZEN_CHECKOUT", native)
        self.assertEqual(REUSABLE.count("uses: actions/checkout@"), 1)
        self.assertEqual(REUSABLE.count("ref: ${{ inputs.commit }}"), 1)
        self.assertIn('run: echo "commit=$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"', job(CALLER, "record-frozen-commit"))

    def test_environment_runners_timeouts_and_branch_gate(self):
        call = job(CALLER, "windows-cross")
        self.assertEqual(value(call, "if", 4), "${{ vars.SKIP_BRANCH_PUSH != 'true' || github.event_name == 'pull_request' }}")
        self.assertEqual(section(REUSABLE, "env", 0).strip(), section(CALLER, "env", 0).split("#", 1)[0].strip())
        self.assertEqual(value(section(section(REUSABLE, "defaults", 0), "run", 2), "shell", 4), "bash")
        for name, runner in (("windows-cross", "ubuntu-latest"), ("windows-native-test", "windows-2022")):
            block = job(REUSABLE, name)
            self.assertEqual(value(block, "runs-on", 4), runner)
            self.assertEqual(value(block, "timeout-minutes", 4), "360")
        build_env = section(job(REUSABLE, "windows-cross"), "env", 4)
        self.assertEqual(value(build_env, "FILE_ENV", 6), "${{ inputs.file-env }}")
        self.assertEqual(value(build_env, "DANGER_CI_ON_HOST_FOLDERS", 6), "1")
        self.assertEqual(value(build_env, "CI_PRUNE_BUILDX_AFTER_IMAGE", 6), "1")
        native_env = section(job(REUSABLE, "windows-native-test"), "env", 4)
        self.assertEqual(value(native_env, "PYTHONUTF8", 6), "1")
        self.assertEqual(value(native_env, "TEST_RUNNER_TIMEOUT_FACTOR", 6), "40")

    def test_no_new_permissions_secrets_events_or_yaml_merge_keys(self):
        for source in (job(CALLER, "windows-cross"), REUSABLE):
            self.assertNotRegex(source, r"(?m)^\s*(permissions|secrets|environment|<<):")
        self.assertEqual(re.findall(r"^  ([\w-]+):", section(REUSABLE, "on", 0), re.M), ["workflow_call"])
        # Inputs may select checkout/action/env metadata, never shell source,
        # including a line inside a multiline run block.
        for line in REUSABLE.splitlines():
            if "${{ inputs." in line:
                self.assertRegex(line, r"^\s+(name|ref|FILE_ENV|CRT): ")

    def test_cache_actions_build_and_artifacts_preserved(self):
        build = job(REUSABLE, "windows-cross")
        uses = re.findall(r"^        uses: (.+)$", build, re.M)
        self.assertEqual(uses, ["actions/checkout@v6", "./.github/actions/configure-environment", "./.github/actions/cache/restore",
                                "./.github/actions/configure-docker", "./.github/actions/cache/save", "actions/upload-artifact@v7"])
        self.assertEqual(build.count("provider: gha"), 3)
        self.assertEqual(build.count("run: ./ci/test_run_all.sh"), 1)
        self.assertIn("if: ${{ (success() || failure()) && github.event_name == 'push' }}", build)
        artifact = "name: ${{ inputs.artifact-name }}-${{ github.run_id }}"
        self.assertEqual(build.count(artifact), 1)
        native = job(REUSABLE, "windows-native-test")
        self.assertIn("uses: actions/download-artifact@v8", native)
        self.assertEqual(native.count(artifact), 1)
        self.assertNotIn("overwrite:", REUSABLE)
        self.assertNotIn("continue-on-error:", REUSABLE)
        self.assertEqual(re.findall(r"^            \$\{\{ env.BASE_BUILD_DIR \}\}/(.+)$", build, re.M),
                         ["bin/*.dll", "bin/*.exe", "src/secp256k1/bin/*.exe", "src/univalue/*.exe", "test/config.ini"])
        self.assertEqual(len({artifact for _, artifact in CRTS.values()}), len(CRTS))

    def test_native_tooling_tests_extended_policy_and_annotations_preserved(self):
        native = job(REUSABLE, "windows-native-test")
        calls = re.findall(r"py -3 \.github/ci-windows-cross.py (\w+)", native)
        self.assertEqual(calls, ["print_version", "check_imports", "check_manifests", "prepare_tests", "run_unit_tests", "run_functional_tests"])
        self.assertIn('run: py -3 .github/ci-windows.py "standard" github_import_vs_env', native)
        self.assertIn("CRT: ${{ inputs.crt }}", native)
        self.assertIn('TEST_RUNNER_EXTRA: "--timeout-factor=${{ env.TEST_RUNNER_TIMEOUT_FACTOR }} ${{ case(github.event_name == \'pull_request\', \'\', \'--extended\') }}"', native)
        self.assertIn("- &ANNOTATION_PR_NUMBER", job(REUSABLE, "windows-cross"))
        self.assertIn('echo "::notice title=debug_pull_request_number_str::${{ github.event.number }}"', REUSABLE)
        self.assertIn("- *ANNOTATION_PR_NUMBER", native)


if __name__ == "__main__":
    unittest.main()
