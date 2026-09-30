# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Offline regression tests for Windows CI binary-cache refresh decisions."""

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


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


if __name__ == "__main__":
    unittest.main()
