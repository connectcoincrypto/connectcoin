# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regression tests for functional-runner failure diagnostics, without nodes."""

from collections import deque
from concurrent import futures
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from test_runner import TestHandler, get_combined_log_tail


class TestRunnerProgress(unittest.TestCase):
    def make_handler(self, *, use_term_control=False):
        with patch('test_runner.futures.ThreadPoolExecutor'):
            return TestHandler(
                num_tests_parallel=1, tests_dir='', tmpdir='unused',
                test_list=deque(['feature_dbcrash.py', 'example_test.py']),
                flags=['--quiet'], use_term_control=use_term_control,
            )

    def test_heartbeat_is_throttled_and_uses_monotonic_elapsed(self):
        with patch('test_runner.time.monotonic', return_value=100) as clock, patch('builtins.print') as output:
            handler = self.make_handler()
            first, second = futures.Future(), futures.Future()
            handler.jobs = {first: ('z_test.py --variant', 100), second: ('a_test.py', 110)}
            clock.return_value = 129.999
            self.assertFalse(handler.print_progress())
            output.assert_not_called()
            clock.return_value = 130
            self.assertTrue(handler.print_progress())
            output.assert_called_once_with(
                'Progress: 2 running [a_test.py (20 s), z_test.py --variant (30 s)]; 2 queued', flush=True,
            )
            self.assertFalse(handler.print_progress())
            clock.return_value = 159.999
            self.assertFalse(handler.print_progress())
            # Completed jobs disappear, and elapsed time remains per process.
            del handler.jobs[second]
            clock.return_value = 160
            self.assertTrue(handler.print_progress())
            self.assertEqual(output.call_count, 2)
            output.assert_called_with('Progress: 1 running [z_test.py --variant (60 s)]; 2 queued', flush=True)
            handler.jobs.clear()
            clock.return_value = 190
            self.assertFalse(handler.print_progress())
            self.assertEqual(output.call_count, 2)

    def test_wait_loop_reports_without_waiting_or_changing_scheduling(self):
        with (
            patch('test_runner.time.monotonic', return_value=0) as clock,
            patch('test_runner.time.time', return_value=100),
            patch('test_runner.subprocess.Popen', return_value=Mock(returncode=0)) as popen,
            patch('builtins.print') as output,
        ):
            handler = self.make_handler()
            future = futures.Future()
            handler.executor.submit.return_value = future
            ticks = iter([30, 59.999, 60, 60.5])

            def wait(active, *, timeout, return_when):
                self.assertEqual(list(active), [future])
                self.assertEqual(timeout, 0.5)
                self.assertEqual(return_when, futures.FIRST_COMPLETED)
                clock.return_value = next(ticks)
                if clock.return_value == 60.5:
                    future.set_result(handler.executor.submit.call_args.args[1])
                    return SimpleNamespace(done={future}, not_done=set())
                return SimpleNamespace(done=set(), not_done={future})

            with patch('test_runner.futures.wait', side_effect=wait):
                result = handler.get_next()
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0][0].name, 'feature_dbcrash.py')
            self.assertEqual(result[0][0].status, 'Passed')
            self.assertEqual(list(handler.test_list), ['example_test.py'])
            self.assertEqual(handler.jobs, {})
            popen.assert_called_once()
            self.assertIn('--quiet', popen.call_args.args[0])
            self.assertEqual(output.call_count, 2)
            output.assert_any_call('Progress: 1 running [feature_dbcrash.py (30 s)]; 1 queued', flush=True)
            output.assert_any_call('Progress: 1 running [feature_dbcrash.py (60 s)]; 1 queued', flush=True)

    def test_terminal_heartbeat_clears_dots(self):
        with patch('test_runner.time.monotonic', return_value=0) as clock, patch('builtins.print') as output:
            handler = self.make_handler(use_term_control=True)
            handler.jobs = {futures.Future(): ('feature_dbcrash.py', 0)}
            clock.return_value = 30
            self.assertTrue(handler.print_progress(dot_count=3))
            output.assert_called_once_with(
                '\r   \rProgress: 1 running [feature_dbcrash.py (30 s)]; 2 queued', flush=True,
            )


class TestCombinedLogs(unittest.TestCase):
    def test_encodings_and_empty_logs(self):
        tests_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='connectcoin-log-test-') as directory:
            root = Path(directory)
            (root / 'test_framework.log').write_text('2026-09-08T00:00:00Z Conexão: ação 世界\n', encoding='utf-8')
            node = root / 'node0'
            (node / 'regtest').mkdir(parents=True)
            (node / 'stderr').mkdir()
            (node / 'regtest' / 'debug.log').write_bytes(b'')
            (node / 'stderr' / 'error.log').write_bytes(b'Windows: conex\xe3o indispon\xedvel\n')
            # The runner's pipe contract must override even an explicit legacy
            # encoding, rather than rely on the developer's current locale.
            with patch.dict(os.environ, {'PYTHONIOENCODING': 'cp1252', 'PYTHONUTF8': '0'}):
                text = get_combined_log_tail(tests_dir, directory, 20)
            self.assertIn('Conexão: ação 世界', text)
            self.assertIn(r'Windows: conex\xe3o indispon\xedvel', text)

            (node / 'regtest' / 'debug.log').write_bytes(b' \t\n  \n')
            self.assertEqual(get_combined_log_tail(tests_dir, directory, 20), text)

            (node / 'regtest' / 'debug.log').write_bytes(b'2026-09-08T00:00:01Z invalid byte: \xff\n')
            text = get_combined_log_tail(tests_dir, directory, 20, color=True)
            self.assertIn(r'invalid byte: \xff', text)
            self.assertIn('\033[', text)
            self.assertEqual(get_combined_log_tail(tests_dir, directory, 2),
                             '\n'.join(get_combined_log_tail(tests_dir, directory, 20).splitlines()[-2:]))
            self.assertEqual(get_combined_log_tail(tests_dir, directory, 0), '')

    def test_failed_combination_is_reported(self):
        tests_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='connectcoin-log-test-') as directory:
            # Two chain directories are deliberately ambiguous. A diagnostic
            # failure must be reported, but must not raise out of the runner.
            for chain in ('regtest', 'testnet4'):
                path = Path(directory) / 'node0' / chain
                path.mkdir(parents=True)
                (path / 'debug.log').write_bytes(b'')
            with self.assertLogs(level='WARNING') as logs:
                self.assertEqual(get_combined_log_tail(tests_dir, directory, 20), '')
            self.assertIn('Log combination exited with status 1', logs.output[0])
