#!/usr/bin/env python3
# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Offline regressions for pinned QA assets acquisition and interrupted Git."""

from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import MagicMock, patch

import qa_assets


class RetryTests(unittest.TestCase):
    @staticmethod
    def process(returncode=0):
        process = MagicMock()
        process.returncode = returncode
        return process

    def test_retry_failure_and_bound_attempts(self):
        for operation in ("fetch", "checkout"):
            for exit_codes, success in (([128, 0], True), ([128, 128, 128], False)):
                with self.subTest(operation=operation, exit_codes=exit_codes):
                    processes = [self.process(code) for code in exit_codes]
                    with patch.object(qa_assets.subprocess, "Popen", side_effect=processes) as popen, patch.object(qa_assets.time, "sleep") as sleep:
                        if success:
                            qa_assets.git([operation, "pinned-commit"], network=True)
                        else:
                            with self.assertRaises(subprocess.CalledProcessError):
                                qa_assets.git([operation, "pinned-commit"], network=True)
                    self.assertEqual(popen.call_count, len(exit_codes))
                    self.assertEqual(sleep.call_count, len(exit_codes) - 1)
                    self.assertEqual(sleep.call_args_list[0].args, (2,))
                    for process in processes:
                        process.wait.assert_called_once_with(timeout=600)
                    for invocation in popen.call_args_list:
                        self.assertEqual(invocation.kwargs["env"]["GIT_HTTP_LOW_SPEED_TIME"], "60")
                        self.assertEqual(invocation.kwargs["env"]["GIT_HTTP_LOW_SPEED_LIMIT"], "1024")
                        self.assertIn("http.version=HTTP/1.1", invocation.args[0])

    def test_forced_timeout_reaps_process_tree_without_blind_retry(self):
        failed = self.process()
        failed.wait.side_effect = subprocess.TimeoutExpired("git", 600)
        with patch.object(qa_assets.subprocess, "Popen", return_value=failed) as popen, patch.object(qa_assets, "stop_process_tree") as stop, patch.object(qa_assets.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "may leave Git locks"):
                qa_assets.git(["checkout", "pin"], network=True)
        stop.assert_called_once_with(failed)
        self.assertEqual(popen.call_count, 1)
        sleep.assert_not_called()
        failed.__exit__.assert_not_called()

    def test_local_failure_is_not_retried(self):
        process = self.process(1)
        with patch.object(qa_assets.subprocess, "Popen", return_value=process) as popen, patch.object(qa_assets.time, "sleep") as sleep:
            with self.assertRaises(subprocess.CalledProcessError):
                qa_assets.git(["remote", "get-url", "origin"], capture=True)
        self.assertEqual(popen.call_count, 1)
        process.wait.assert_called_once_with(timeout=60)
        sleep.assert_not_called()

    def test_failed_tree_termination_stops_retry(self):
        process = self.process()
        process.wait.side_effect = subprocess.TimeoutExpired("git", 600)
        with patch.object(qa_assets.subprocess, "Popen", return_value=process) as popen, patch.object(qa_assets, "stop_process_tree", side_effect=RuntimeError("tree cleanup failed")), patch.object(qa_assets.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "tree cleanup failed"):
                qa_assets.git(["fetch", "pin"], network=True)
        self.assertEqual(popen.call_count, 1)
        sleep.assert_not_called()

    def test_windows_taskkill_failure_kills_parent_and_reports_failure(self):
        for failure in (OSError("taskkill failed"), subprocess.TimeoutExpired("taskkill", 15)):
            with self.subTest(failure=failure):
                process = self.process()
                process.poll.return_value = None
                with patch.object(qa_assets.os, "name", "nt"), patch.object(qa_assets.subprocess, "run", side_effect=failure):
                    with self.assertRaisesRegex(RuntimeError, "process tree"):
                        qa_assets.stop_process_tree(process)
                process.kill.assert_called_once()
                process.wait.assert_called_once_with(timeout=15)

    def test_capture_does_not_wait_for_descendant_output_eof(self):
        # The parent exits while its child keeps inherited stdout/stderr open.
        # PIPE-backed capture would wait for the fallback deadline; file-backed
        # capture returns while the child is waiting for our release signal.
        with tempfile.TemporaryDirectory(prefix="qa assets output ") as temporary:
            directory = Path(temporary)
            ready, release, done = (directory / name for name in ("ready", "release", "done"))
            child = (
                f"import pathlib,time; ready=pathlib.Path({str(ready)!r}); release=pathlib.Path({str(release)!r}); "
                f"done=pathlib.Path({str(done)!r}); ready.touch(); deadline=time.monotonic()+10\n"
                "while not release.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                "done.touch()\n"
            )
            parent = (
                f"import pathlib,subprocess,sys,time; subprocess.Popen([sys.executable, '-c', {child!r}]); "
                f"ready=pathlib.Path({str(ready)!r}); deadline=time.monotonic()+10\n"
                "while not ready.exists() and time.monotonic()<deadline: time.sleep(0.01)\n"
                "print('parent done')\n"
            )
            real_popen = subprocess.Popen
            children = []

            def spawn(_command, **kwargs):
                process = real_popen([sys.executable, "-c", parent], **kwargs)
                children.append(process)
                return process

            try:
                with patch.object(qa_assets.subprocess, "Popen", side_effect=spawn):
                    self.assertEqual(qa_assets.git(["status"], capture=True), "parent done")
                self.assertTrue(ready.exists())
                self.assertFalse(done.exists(), "capture waited for the descendant's output EOF")
                self.assertIsNone(children[0].stdout)
                self.assertIsNone(children[0].stderr)
            finally:
                release.touch()
                deadline = time.monotonic() + 5
                while not done.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(done.exists(), "test descendant did not exit after release")

    def test_real_timeout_terminates_descendant(self):
        with tempfile.TemporaryDirectory(prefix="qa assets timeout ") as temporary:
            marker = Path(temporary) / "orphan-ran"
            child = f"import pathlib,time; time.sleep(2); pathlib.Path({str(marker)!r}).write_text('orphan')"
            parent = f"import subprocess,sys,time; subprocess.Popen([sys.executable, '-c', {child!r}]); time.sleep(30)"
            real_popen = subprocess.Popen

            def spawn(_command, **kwargs):
                return real_popen([sys.executable, "-c", parent], **kwargs)

            started = time.monotonic()
            with patch.object(qa_assets.subprocess, "Popen", side_effect=spawn), patch.object(qa_assets, "NETWORK_TIMEOUT", 0.3), patch.object(qa_assets, "NETWORK_ATTEMPTS", 1):
                # taskkill uses subprocess.run/Popen too; bypass only that call.
                real_run = subprocess.run

                def run_cleanup(command, **kwargs):
                    with patch.object(qa_assets.subprocess, "Popen", real_popen):
                        return real_run(command, **kwargs)

                with patch.object(qa_assets.subprocess, "run", side_effect=run_cleanup), self.assertRaisesRegex(RuntimeError, "may leave Git locks"):
                    qa_assets.git(["fetch", "pin"], network=True, capture=True)
            self.assertLess(time.monotonic() - started, 10)
            time.sleep(2.2)
            self.assertFalse(marker.exists(), "a Git descendant survived timeout")


class CheckoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qa assets checkout ")
        self.addCleanup(self.cleanup_directory)
        self.root = Path(self.temporary.name)
        self.remote = self.root / "source"
        self.directory = self.root / "nested path" / "checkout"
        self.command("init", self.remote)
        self.command("-C", self.remote, "config", "user.name", "QA test")
        self.command("-C", self.remote, "config", "user.email", "qa@example.invalid")
        corpus = self.remote / "fuzz_corpora" / "sample"
        corpus.mkdir(parents=True)
        (corpus / "one").write_bytes(b"pinned corpus input\x00")
        (self.remote / "README").write_text("pinned ancillary file", encoding="utf8")
        self.command("-C", self.remote, "add", ".")
        self.command("-C", self.remote, "commit", "-m", "pinned")
        self.pin = self.command("-C", self.remote, "rev-parse", "HEAD")
        (corpus / "two").write_bytes(b"new moving HEAD input")
        self.command("-C", self.remote, "add", ".")
        self.command("-C", self.remote, "commit", "-m", "later")
        self.tip = self.command("-C", self.remote, "rev-parse", "HEAD")
        self.url = self.remote.as_uri()
        self.output = io.StringIO()
        self.redirect = redirect_stdout(self.output)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)

    def cleanup_directory(self):
        # Windows TerminateProcess can return before killed descendants release
        # their file handles. Retry only cleanup of this test's owned fixture;
        # the production helper deliberately preserves any surviving Git lock.
        deadline = time.monotonic() + 5
        while True:
            try:
                self.temporary.cleanup()
                return
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

    @staticmethod
    def command(*arguments):
        return subprocess.run(["git", *map(str, arguments)], check=True, text=True, capture_output=True).stdout.strip()

    def acquire(self, pin=None):
        qa_assets.acquire(self.directory, pin or self.pin, remote=self.url)

    def test_exact_pin_complete_tree_and_reusable_checkout(self):
        self.acquire()
        self.assertEqual(self.command("-C", self.directory, "rev-parse", "HEAD"), self.pin)
        self.assertEqual(self.command("-C", self.directory, "rev-list", "--count", "HEAD"), "1")
        self.assertEqual((self.directory / "fuzz_corpora/sample/one").read_bytes(), b"pinned corpus input\x00")
        self.assertEqual((self.directory / "README").read_text(encoding="utf8"), "pinned ancillary file")
        self.assertFalse((self.directory / "fuzz_corpora/sample/two").exists())
        seed = self.directory / "fuzz_corpora/sample/generated-seed"
        seed.write_bytes(b"generated")
        with patch.object(qa_assets, "git", wraps=qa_assets.git) as operations:
            self.acquire()
        self.assertFalse(any("fetch" in operation.args[0] for operation in operations.call_args_list))
        self.assertEqual(seed.read_bytes(), b"generated")

    def test_matching_head_no_checkout_is_materialized(self):
        self.directory.parent.mkdir(parents=True)
        self.command("clone", "--no-checkout", self.url, self.directory)
        self.assertEqual(self.command("-C", self.directory, "rev-parse", "HEAD"), self.tip)
        self.assertFalse((self.directory / "fuzz_corpora").exists())
        with patch.object(qa_assets, "git", wraps=qa_assets.git) as operations:
            self.acquire(self.tip)
        self.assertFalse(any("fetch" in operation.args[0] for operation in operations.call_args_list))
        self.assertEqual((self.directory / "fuzz_corpora/sample/two").read_bytes(), b"new moving HEAD input")

    def test_unborn_repository_fetches_pin(self):
        self.command("init", self.directory)
        self.command("-C", self.directory, "remote", "add", "origin", self.url)
        with patch.object(qa_assets, "git", wraps=qa_assets.git) as operations:
            self.acquire()
        self.assertTrue(any("fetch" in operation.args[0] for operation in operations.call_args_list))
        self.assertEqual(self.command("-C", self.directory, "rev-parse", "HEAD"), self.pin)
        self.assertEqual((self.directory / "fuzz_corpora/sample/one").read_bytes(), b"pinned corpus input\x00")

    def test_different_head_fetches_requested_pin(self):
        self.acquire()
        with patch.object(qa_assets, "git", wraps=qa_assets.git) as operations:
            self.acquire(self.tip)
        self.assertTrue(any("fetch" in operation.args[0] for operation in operations.call_args_list))
        self.assertEqual(self.command("-C", self.directory, "rev-parse", "HEAD"), self.tip)
        self.assertEqual((self.directory / "fuzz_corpora/sample/two").read_bytes(), b"new moving HEAD input")

    def test_tracked_edits_deletions_and_index_changes_fail_closed(self):
        for change in ("deletion", "modification", "staged-deletion", "staged-modification", "staged-addition"):
            with self.subTest(change=change):
                self.directory = self.root / change
                self.acquire()
                tracked = self.directory / "fuzz_corpora/sample/one"
                if "deletion" in change:
                    tracked.unlink()
                elif change == "staged-addition":
                    (self.directory / "fuzz_corpora/sample/added").write_bytes(b"staged new input")
                else:
                    tracked.write_bytes(b"local modification")
                if change.startswith("staged-"):
                    self.command("-C", self.directory, "add", "-A")
                before = self.command("-C", self.directory, "status", "--porcelain")
                with self.assertRaisesRegex(RuntimeError, "tracked changes or missing files"):
                    self.acquire()
                self.assertEqual(self.command("-C", self.directory, "status", "--porcelain"), before)
                if "deletion" not in change and change != "staged-addition":
                    self.assertEqual(tracked.read_bytes(), b"local modification")

    def test_index_flags_cannot_hide_missing_or_modified_corpus(self):
        for flag in ("--skip-worktree", "--assume-unchanged"):
            with self.subTest(flag=flag):
                self.directory = self.root / flag
                self.acquire()
                self.command("-C", self.directory, "update-index", flag, "fuzz_corpora/sample/one")
                (self.directory / "fuzz_corpora/sample/one").write_bytes(b"hidden modification")
                self.assertEqual(self.command("-C", self.directory, "status", "--porcelain"), "")
                with self.assertRaisesRegex(RuntimeError, "index hides tracked files"):
                    self.acquire()

    def test_interrupted_fetch_and_checkout_retry_with_real_git(self):
        for operation in ("fetch", "checkout"):
            with self.subTest(operation=operation):
                self.directory = self.root / operation
                real_popen = subprocess.Popen
                interrupted = []

                def spawn(command, **kwargs):
                    if operation in command and not interrupted:
                        interrupted.append(command)
                        return real_popen([sys.executable, "-c", "import sys; sys.exit(128)"], **kwargs)
                    return real_popen(command, **kwargs)

                with patch.object(qa_assets.subprocess, "Popen", side_effect=spawn), patch.object(qa_assets.time, "sleep"):
                    self.acquire()
                self.assertEqual(len(interrupted), 1)
                self.assertEqual(self.command("-C", self.directory, "rev-parse", "HEAD"), self.pin)
                self.assertTrue((self.directory / "fuzz_corpora/sample/one").is_file())

    def test_unavailable_pin_fails_without_falling_back_to_head(self):
        with patch.object(qa_assets.time, "sleep"), self.assertRaises(subprocess.CalledProcessError):
            self.acquire("b" * 40)
        self.assertFalse((self.directory / "fuzz_corpora").exists())

    def test_real_checkout_timeout_preserves_lock_and_does_not_retry(self):
        (self.remote / ".gitattributes").write_text("fuzz_corpora/** filter=delay\n", encoding="utf8")
        self.command("-C", self.remote, "add", ".gitattributes")
        self.command("-C", self.remote, "commit", "-m", "slow checkout fixture")
        self.pin = self.command("-C", self.remote, "rev-parse", "HEAD")
        self.command("init", self.directory)
        self.command("-C", self.directory, "remote", "add", "origin", self.url)
        self.command("-C", self.directory, "fetch", "--depth=1", "origin", self.pin)
        script = "import sys,time; time.sleep(4); sys.stdout.buffer.write(sys.stdin.buffer.read())"
        filter_command = f'"{Path(sys.executable).as_posix()}" -c "{script}"'
        self.command("-C", self.directory, "config", "filter.delay.smudge", filter_command)
        with patch.object(qa_assets, "NETWORK_TIMEOUT", 1), patch.object(qa_assets.subprocess, "Popen", wraps=subprocess.Popen) as popen:
            with self.assertRaisesRegex(RuntimeError, "may leave Git locks"):
                qa_assets.git(["-C", self.directory, "checkout", "--detach", self.pin], network=True)
        git_calls = [item for item in popen.call_args_list if item.args[0][0] == "git"]
        self.assertEqual(len(git_calls), 1)
        self.assertTrue((self.directory / ".git/index.lock").exists())

    def test_preexisting_git_lock_is_not_deleted(self):
        self.acquire()
        lock = self.directory / ".git/index.lock"
        lock.write_bytes(b"another Git operation")
        with patch.object(qa_assets.time, "sleep"), self.assertRaises(subprocess.CalledProcessError):
            self.acquire()
        self.assertEqual(lock.read_bytes(), b"another Git operation")
        self.assertEqual((self.directory / "fuzz_corpora/sample/one").read_bytes(), b"pinned corpus input\x00")

    def test_invalid_pin_does_not_touch_path(self):
        for pin in ("HEAD", "--upload-pack=evil", "b" * 39, "B" * 40):
            with self.subTest(pin=pin), self.assertRaises(ValueError):
                self.acquire(pin)
        self.assertFalse(self.directory.exists())

    def test_nonrepository_path_is_preserved(self):
        self.directory.mkdir(parents=True)
        marker = self.directory / "sentinel"
        marker.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "not a Git checkout"):
            self.acquire()
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_foreign_origin_is_preserved(self):
        self.command("init", self.directory)
        self.command("-C", self.directory, "remote", "add", "origin", "https://example.invalid/foreign")
        with self.assertRaisesRegex(ValueError, "unexpected origin"):
            self.acquire()
        self.assertEqual(self.command("-C", self.directory, "remote", "get-url", "origin"), "https://example.invalid/foreign")

    def test_symlink_is_rejected(self):
        self.directory.parent.mkdir(parents=True)
        try:
            self.directory.symlink_to(self.remote, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.acquire()
        self.assertEqual(self.command("-C", self.remote, "rev-parse", "HEAD"), self.tip)


if __name__ == "__main__":
    unittest.main()
