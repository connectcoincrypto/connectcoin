# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regression tests for functional-runner failure diagnostics, without nodes."""

import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_runner import get_combined_log_tail


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
