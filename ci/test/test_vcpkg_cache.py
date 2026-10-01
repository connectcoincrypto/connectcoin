# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Offline regression tests for Windows CI binary-cache refresh decisions."""

import importlib.util
from contextlib import chdir
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import call, patch
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_windows", REPO_ROOT / ".github" / "ci-windows.py")
CI_WINDOWS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI_WINDOWS)
ABI_SPEC = importlib.util.spec_from_file_location("vcpkg_abi_diagnostics", REPO_ROOT / "ci/test/vcpkg_abi_diagnostics.py")
ABI_DIAGNOSTICS = importlib.util.module_from_spec(ABI_SPEC)
ABI_SPEC.loader.exec_module(ABI_DIAGNOSTICS)


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


class VcpkgAbiDiagnosticsTests(unittest.TestCase):
    def test_distinguishes_absent_cache_from_changed_abi_and_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archives = root / "archives"
            archives.mkdir()
            buildtrees = root / "buildtrees"
            port = buildtrees / "qtbase"
            port.mkdir(parents=True)
            old = b"triplet x64-windows-release\ncmake 4.3.2\nfeatures core,gui\n"
            current = b"triplet x64-windows-release\ncmake 4.4.3\nfeatures core,gui\n"
            (port / "x64-windows-release.vcpkg_abi_info.txt").write_bytes(current)
            empty = ABI_DIAGNOSTICS.collect(archives)
            after = ABI_DIAGNOSTICS.collect(archives, buildtrees)
            self.assertEqual(ABI_DIAGNOSTICS.compare(empty, after)[0]["cache_status"], "not_cached")
            for data in (old, current):
                abi = hashlib.sha256(data).hexdigest()
                with zipfile.ZipFile(archives / f"{abi}.zip", "w") as archive:
                    archive.writestr("share/qtbase/vcpkg_abi_info.txt", data)
                before = ABI_DIAGNOSTICS.collect(archives)
                comparison = ABI_DIAGNOSTICS.compare(before, after)[0]
                if data == old:
                    self.assertEqual(comparison["cache_status"], "abi_mismatch")
                    self.assertEqual(comparison["differences"][0]["changed_inputs"],
                                     {"cmake": {"cached": "4.3.2", "current": "4.4.3"}})
                else:
                    self.assertEqual(comparison["cache_status"], "match")
                    self.assertEqual(comparison["differences"], [])
            self.assertEqual(before["archive_count"], 2)
            self.assertEqual(before["archive_bytes"], sum(path.stat().st_size for path in archives.iterdir()))

    def test_metadata_filters_arbitrary_values_and_archive_members(self):
        digest = "a" * 64
        data = (f"portfile.cmake {digest}\ntriplet x64-windows-release\n"
                "powershell 7.6.6\nsecret token-value\ncmake token-value\nfeatures token value\n").encode()
        self.assertEqual(ABI_DIAGNOSTICS.abi_entries(data),
                         {"portfile.cmake": digest, "triplet": "x64-windows-release", "powershell": "7.6.6"})
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with zipfile.ZipFile(root / f"{hashlib.sha256(data).hexdigest()}.zip", "w") as archive:
                archive.writestr("share/qtbase/vcpkg_abi_info.txt", data)
                archive.writestr("../secret/vcpkg_abi_info.txt", b"arbitrary secret")
                archive.writestr("build.log", b"arbitrary secret")
                archive.writestr("share/huge/vcpkg_abi_info.txt", b"x" * (ABI_DIAGNOSTICS.MAX_ABI_BYTES + 1))
            (root / "broken.zip").write_bytes(b"arbitrary secret")
            report = ABI_DIAGNOSTICS.collect(root)
            self.assertEqual(len(report["cached"]), 1)
            self.assertEqual(len(report["errors"]), 1)
            self.assertNotIn("secret", json.dumps(report))
            self.assertEqual(len(list(root.iterdir())), 2)  # ZIP contents are never extracted.

    def test_rejects_misnamed_or_inconsistent_cached_abi(self):
        old = b"triplet x64-windows-release\ncmake 4.3.2\n"
        current = b"triplet x64-windows-release\ncmake 4.4.3\n"
        current_abi = hashlib.sha256(current).hexdigest()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("not-an-abi", current_abi):
                with zipfile.ZipFile(root / f"{name}.zip", "w") as archive:
                    archive.writestr("share/qtbase/vcpkg_abi_info.txt", old)
            before = ABI_DIAGNOSTICS.collect(root)
            after = {"current": [{"port": "qtbase", "abi": current_abi,
                                  "entries": ABI_DIAGNOSTICS.abi_entries(current)}]}
            self.assertEqual(before["cached"], [])
            self.assertEqual(len(before["errors"]), 2)
            self.assertEqual(ABI_DIAGNOSTICS.compare(before, after)[0]["cache_status"], "not_cached")

    def test_unreadable_or_disappearing_metadata_does_not_abort_collection(self):
        data = b"triplet x64-windows-release\ncmake 4.3.2\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archives = root / "archives"
            archives.mkdir()
            archive_path = archives / f"{hashlib.sha256(data).hexdigest()}.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("share/qtbase/vcpkg_abi_info.txt", data)
            buildtrees = root / "buildtrees"
            current_path = buildtrees / "qtbase/x64-windows-release.vcpkg_abi_info.txt"
            current_path.parent.mkdir(parents=True)
            current_path.write_bytes(data)
            original_stat = Path.stat

            def failing_stat(path, *args, **kwargs):
                if path in (archive_path, current_path):
                    raise PermissionError("must not expose this diagnostic")
                return original_stat(path, *args, **kwargs)

            with patch.object(Path, "stat", failing_stat):
                report = ABI_DIAGNOSTICS.collect(archives, buildtrees)
            self.assertEqual(report["cached"], [])
            self.assertEqual(report["current"], [])
            self.assertEqual(len(report["errors"]), 2)
            with patch.object(zipfile, "ZipFile", side_effect=FileNotFoundError("private path")), \
                    patch.object(Path, "read_bytes", side_effect=FileNotFoundError("private path")):
                report = ABI_DIAGNOSTICS.collect(archives, buildtrees)
            self.assertEqual(len(report["errors"]), 2)
            self.assertNotIn("private path", json.dumps(report))

    def test_corrupt_compressed_stream_does_not_abort_collection(self):
        data = b"triplet x64-windows-release\ncmake 4.3.2\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / f"{hashlib.sha256(data).hexdigest()}.zip"
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("share/qtbase/vcpkg_abi_info.txt", data)
                offset = archive.infolist()[0].header_offset
            payload = bytearray(path.read_bytes())
            name_size, extra_size = struct.unpack_from("<HH", payload, offset + 26)
            payload[offset + 30 + name_size + extra_size] = 7  # Invalid DEFLATE block type.
            path.write_bytes(payload)
            report = ABI_DIAGNOSTICS.collect(root)
            self.assertEqual(report["cached"], [])
            self.assertEqual(report["errors"], ["unreadable cached ABI metadata"])
            with patch.object(zipfile, "ZipFile", side_effect=EOFError("private path")):
                report = ABI_DIAGNOSTICS.collect(root)
            self.assertEqual(report["errors"], ["unreadable cached ABI metadata"])

    def test_workflow_captures_both_sides_and_uploads_on_failure(self):
        workflow = (REPO_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf8")
        job = workflow.split("  windows-native-dll:", 1)[1].split("  record-frozen-commit:", 1)[0]
        steps = ["Restore vcpkg binary cache", "Record restored vcpkg ABI inputs", "Generate build system",
                 "Compare vcpkg ABI inputs", "Upload vcpkg ABI diagnostics", "Build"]
        positions = [job.index(f"- name: {step}") for step in steps]
        self.assertEqual(positions, sorted(positions))
        compare = job.split("      - name: Compare vcpkg ABI inputs", 1)[1].split("      - name:", 1)[0]
        upload = job.split("      - name: Upload vcpkg ABI diagnostics", 1)[1].split("      - name:", 1)[0]
        self.assertIn("success() || failure()", compare)
        self.assertIn("steps.vcpkg-configure.outcome == 'failure'", compare)
        self.assertIn("success() || failure()", upload)
        self.assertIn("path: build/ci-vcpkg-abi/*.json", upload)
        self.assertNotIn("continue-on-error:", compare + upload)


if __name__ == "__main__":
    unittest.main()
