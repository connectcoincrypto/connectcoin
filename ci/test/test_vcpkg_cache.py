# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Offline regression tests for Windows CI binary-cache refresh decisions."""

import importlib.util
from contextlib import chdir
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import call, patch


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_windows", REPO_ROOT / ".github" / "ci-windows.py")
CI_WINDOWS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI_WINDOWS)


class VcpkgCacheTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.archives = self.root / "vcpkg" / "archives"

    def fingerprint(self):
        return CI_WINDOWS.vcpkg_cache_fingerprint(self.archives)

    def archive(self, name, content):
        path = self.archives / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_missing_and_empty_cache_match(self):
        missing = self.fingerprint()
        self.archives.mkdir(parents=True)
        self.assertEqual(missing, self.fingerprint())

    def test_new_abi_refreshes_restored_cache(self):
        self.archive("aa/old-abi.zip", b"old ABI")
        before = self.fingerprint()
        self.archive("bb/new-abi.zip", b"new ABI")
        self.assertNotEqual(before, self.fingerprint())

    def test_unchanged_archives_do_not_duplicate_cache(self):
        archive = self.archive("aa/abi.zip", b"reusable")
        before = self.fingerprint()
        os.utime(archive, (1, 1))
        self.archive("incomplete.zip.tmp", b"not a completed archive")
        self.assertEqual(before, self.fingerprint())

    def test_same_size_repair_refreshes_cache(self):
        self.archive("aa/abi.zip", b"broken")
        before = self.fingerprint()
        self.archive("aa/abi.zip", b"usable")
        self.assertNotEqual(before, self.fingerprint())

    def test_order_is_deterministic(self):
        first = self.archive("bb/second.zip", b"second")
        self.archive("aa/first.zip", b"first")
        before = self.fingerprint()
        first.unlink()
        self.archive("bb/second.zip", b"second")
        self.assertEqual(before, self.fingerprint())

    def test_github_output_uses_matching_archive_directory(self):
        self.archive("aa/abi.zip", b"reusable")
        output = self.root / "github-output"
        with patch.dict(os.environ, {"LOCALAPPDATA": str(self.root), "GITHUB_OUTPUT": str(output)}):
            CI_WINDOWS.vcpkg_cache_state("standard")
        self.assertEqual(output.read_text(encoding="utf8"), f"fingerprint={self.fingerprint()}\n")

    def test_workflow_retains_single_push_writer_and_compatibility(self):
        workflow = (REPO_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf8")
        windows_job = workflow.split("  windows-native-dll:", 1)[1].split("  record-frozen-commit:", 1)[0]
        restore = windows_job.split("      - name: Restore vcpkg binary cache", 1)[1].split("      - name:", 1)[0]
        save = windows_job.split("      - name: Save vcpkg binary cache", 1)[1].split("      - name:", 1)[0]
        self.assertIn("-${{ github.run_id }}-${{ github.run_attempt }}", restore)
        self.assertIn("restore-keys: |", restore)
        self.assertIn("${{ github.job }}-vcpkg-binary-release-\n", restore)
        self.assertIn("if: github.event_name == 'push' && matrix.job-type == 'standard'", save)
        self.assertIn("steps.vcpkg-cache-before.outputs.fingerprint != steps.vcpkg-cache-after.outputs.fingerprint", save)
        self.assertNotIn("cache-hit", save)
        self.assertIn("key: ${{ steps.vcpkg-binary-cache.outputs.cache-primary-key }}", save)


class VcpkgToolchainTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="connectcoin vcpkg ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runner_temp = self.root / "runner temp"
        self.runner_temp.mkdir()
        self.installed = self.root / "installed vcpkg"
        self.installed.mkdir()
        (self.installed / "sentinel").write_text("unchanged", encoding="utf8")
        self.output = self.root / "github-env"
        self.baseline = "a" * 40
        self.manifest(self.baseline)
        self.environment = {
            "RUNNER_TEMP": str(self.runner_temp), "GITHUB_ENV": str(self.output),
            "VCPKG_ROOT": str(self.installed), "COMSPEC": "cmd.exe",
        }

    def manifest(self, baseline):
        (self.root / "vcpkg.json").write_text(json.dumps({"builtin-baseline": baseline}), encoding="utf8")

    def test_pinned_toolchain_is_published_after_successful_bootstrap(self):
        checkout = self.runner_temp / "connectcoin-ci-vcpkg"
        with chdir(self.root), patch.dict(os.environ, self.environment), patch.object(CI_WINDOWS, "run") as run:
            CI_WINDOWS.setup_vcpkg("standard")
        self.assertEqual(run.call_args_list, [
            call(["git", "init", str(checkout)]),
            call(["git", "-C", str(checkout), "fetch", "--depth=1", "https://github.com/microsoft/vcpkg.git", self.baseline]),
            call(["git", "-C", str(checkout), "checkout", "--detach", self.baseline]),
            call(["cmd.exe", "/d", "/c", "bootstrap-vcpkg.bat", "-disableMetrics"], cwd=checkout),
            call([str(checkout / "vcpkg.exe"), "version"]),
        ])
        self.assertEqual(self.output.read_text(encoding="utf8"), f"VCPKG_ROOT={checkout}\n")
        self.assertEqual(list(self.installed.iterdir()), [self.installed / "sentinel"])
        self.assertEqual((self.installed / "sentinel").read_text(encoding="utf8"), "unchanged")

    def test_bootstrap_failure_does_not_publish_unusable_toolchain(self):
        def fail_bootstrap(command, **_kwargs):
            if "bootstrap-vcpkg.bat" in command:
                raise SystemExit("bootstrap failed")
        with chdir(self.root), patch.dict(os.environ, self.environment), patch.object(CI_WINDOWS, "run", side_effect=fail_bootstrap):
            with self.assertRaisesRegex(SystemExit, "bootstrap failed"):
                CI_WINDOWS.setup_vcpkg("fuzz")
        self.assertFalse(self.output.exists())

    def test_invalid_baseline_is_rejected_before_any_checkout(self):
        for baseline in ("main", "a" * 39, "-" * 40, None):
            with self.subTest(baseline=baseline):
                self.manifest(baseline)
                with chdir(self.root), patch.dict(os.environ, self.environment), patch.object(CI_WINDOWS, "run") as run:
                    with self.assertRaises(ValueError):
                        CI_WINDOWS.setup_vcpkg("standard")
                run.assert_not_called()
                self.assertFalse(self.output.exists())
                self.assertFalse((self.runner_temp / "connectcoin-ci-vcpkg").exists())

    def test_existing_checkout_is_not_overwritten(self):
        checkout = self.runner_temp / "connectcoin-ci-vcpkg"
        checkout.mkdir()
        sentinel = checkout / "sentinel"
        sentinel.write_text("retain", encoding="utf8")
        with chdir(self.root), patch.dict(os.environ, self.environment), patch.object(CI_WINDOWS, "run") as run:
            with self.assertRaises(FileExistsError):
                CI_WINDOWS.setup_vcpkg("standard")
        run.assert_not_called()
        self.assertEqual(sentinel.read_text(encoding="utf8"), "retain")
        self.assertFalse(self.output.exists())

    def test_workflow_pins_after_vs_import_and_keys_tool_identity(self):
        workflow = (REPO_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf8")
        job = workflow.split("  windows-native-dll:", 1)[1].split("  record-frozen-commit:", 1)[0]
        steps = ["Import Visual Studio env vars", "Set up pinned vcpkg", "Get tool information", "Restore vcpkg binary cache", "Generate build system"]
        positions = [job.index(f"- name: {step}") for step in steps]
        self.assertEqual(positions, sorted(positions))
        setup = job.split("      - name: Set up pinned vcpkg", 1)[1].split("      - name:", 1)[0]
        self.assertNotIn("if:", setup)  # Both standard and fuzz builds use the pin.
        self.assertIn("timeout-minutes: 5", setup)
        self.assertIn('run: py -3 .github/ci-windows.py "standard" setup_vcpkg', setup)
        self.assertIn('"${VCPKG_ROOT}/vcpkg.exe" version | tee vcpkg_version', job)
        self.assertIn('git -C "${VCPKG_ROOT}" rev-parse HEAD | tee vcpkg_commit', job)
        restore = job.split("      - name: Restore vcpkg binary cache", 1)[1].split("      - name:", 1)[0]
        self.assertEqual(restore.count("'vcpkg_version', 'vcpkg_commit', 'vcpkg.json'"), 2)


if __name__ == "__main__":
    unittest.main()
