# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Offline Huge Pages setup tests. Never access the host's /proc/sys controls."""

from contextlib import contextmanager, redirect_stdout
from copy import deepcopy
from dataclasses import replace
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import hugepages as setup


BOOT = '12345678-1234-1234-1234-123456789abc'


def snapshot(pages=0, group=0, **memory):
    values = {'MemTotal': 16 * 1024 * 1024, 'MemAvailable': 12 * 1024 * 1024,
              'Hugepagesize': 2048, 'Hugetlb': pages * 2048, 'HugePages_Total': pages,
              'HugePages_Free': pages, 'HugePages_Rsvd': 0, 'HugePages_Surp': 0}
    values.update(memory)
    return setup.Snapshot(BOOT, pages, group, values)


class FakeKernel:
    def __init__(self, state=None):
        self.state = state or snapshot()
        self.writes = []
        self.partial = False
        self.fail_group = False

    def snapshot(self):
        return deepcopy(self.state)

    def write(self, name, value):
        self.writes.append((name, value))
        if name == 'hugetlb_shm_group' and self.fail_group:
            raise OSError('simulated group write failure')
        if name == 'nr_hugepages':
            if self.partial:
                value -= 10
                self.partial = False
            difference = value - self.state.nr_hugepages
            memory = dict(self.state.memory)
            memory['HugePages_Total'] = value
            memory['HugePages_Free'] += difference
            memory['Hugetlb'] += difference * 2048
            memory['MemAvailable'] -= difference * 2048
            self.state = replace(self.state, nr_hugepages=value, memory=memory)
        else:
            self.state = replace(self.state, hugetlb_shm_group=value)


class MemoryStore:
    def __init__(self):
        self.active = None
        self.history = []
        self.phases = []

    @contextmanager
    def locked(self):
        yield

    def save(self, record, create=False):
        setup.validate_record(record)
        if create and self.active is not None:
            raise FileExistsError('active backup')
        self.active = deepcopy(record)
        self.phases.append(record['phase'])

    def load(self):
        return deepcopy(self.active)

    def archive(self, record, phase):
        saved = deepcopy(record)
        saved['phase'] = phase
        self.history.append(saved)
        self.active = None


class HugePagesTests(unittest.TestCase):
    def test_budget_includes_cache_and_individually_rounded_vms(self):
        one = setup.plan(snapshot(), 1, 4, 1000)
        self.assertEqual(one['pages_budget'], 1040 + 128 + 128 + 4)
        two = setup.plan(snapshot(), 2, 8, 1000)
        self.assertEqual(two['pages_budget'], 2 * (1040 + 128) + 128 + 8)

    def test_existing_free_pool_is_never_reduced(self):
        proposal = setup.plan(snapshot(pages=2000), 1, 4, 1000)
        self.assertEqual(proposal['additional_pages'], 0)
        self.assertEqual(proposal['target']['nr_hugepages'], 2000)

    def test_used_and_reserved_pages_are_not_counted_as_available(self):
        proposal = setup.plan(snapshot(pages=2000, HugePages_Free=1000, HugePages_Rsvd=100), 1, 4, 1000)
        self.assertEqual(proposal['additional_pages'], 1300 - 900)
        self.assertEqual(proposal['target']['nr_hugepages'], 2400)

    def test_rejects_unsafe_parameters(self):
        for datasets, vms, gid in ((0, 1, 1000), (3, 1, 1000), (True, 1, 1000), (1, 0, 1000),
                                   (1, 1025, 1000), (1, 1, 0), (1, 1, -1), (1, 1, 2**32 - 1)):
            with self.subTest((datasets, vms, gid)), self.assertRaises(setup.SetupError):
                setup.plan(snapshot(), datasets, vms, gid)

    def test_rejects_small_ram_or_low_available_memory(self):
        for state in (snapshot(MemTotal=4 * 1024 * 1024, MemAvailable=4 * 1024 * 1024),
                      snapshot(MemAvailable=3 * 1024 * 1024)):
            with self.assertRaises(setup.SetupError):
                setup.plan(state, 1, 1, 1000)

    def test_rejects_unsupported_or_inconsistent_kernel_state(self):
        for changes in ({'Hugepagesize': 1048576}, {'HugePages_Surp': 1}, {'HugePages_Total': 1},
                        {'HugePages_Rsvd': 1}, {'MemAvailable': 0}, {'MemAvailable': 20 * 1024 * 1024}):
            with self.subTest(changes), self.assertRaises(setup.SetupError):
                setup.plan(snapshot(**changes), 1, 1, 1000)

    def test_preserves_another_groups_access(self):
        with self.assertRaisesRegex(setup.SetupError, 'Another non-root'):
            setup.plan(snapshot(group=2000), 1, 1, 1000)
        self.assertEqual(setup.plan(snapshot(group=1000), 1, 1, 1000)['target']['hugetlb_shm_group'], 1000)

    def test_apply_and_restore_original_nonzero_values(self):
        kernel, store = FakeKernel(snapshot(pages=200, group=1000)), MemoryStore()
        record = setup.apply(kernel, store, 1, 4, 1000)
        self.assertEqual(record['before'], {'nr_hugepages': 200, 'hugetlb_shm_group': 1000})
        self.assertEqual(kernel.writes, [('nr_hugepages', 1300)])
        self.assertEqual(store.phases[:2], ['prepared', 'writing_pages'])
        self.assertIn('Original runtime settings restored', setup.restore(kernel, store))
        self.assertEqual(kernel.state.controls(), record['before'])
        self.assertIsNone(store.active)
        self.assertEqual(store.history[-1]['phase'], 'restored')

    def test_group_grant_is_last_and_original_group_is_restored(self):
        kernel, store = FakeKernel(), MemoryStore()
        setup.apply(kernel, store, 1, 1, 1000)
        self.assertEqual(kernel.writes, [('nr_hugepages', 1297), ('hugetlb_shm_group', 1000)])
        setup.restore(kernel, store)
        self.assertEqual(kernel.writes[-2:], [('hugetlb_shm_group', 0), ('nr_hugepages', 0)])

    def test_active_backup_is_never_overwritten(self):
        kernel, store = FakeKernel(), MemoryStore()
        setup.apply(kernel, store, 1, 1, 1000)
        before, writes = deepcopy(store.active), list(kernel.writes)
        with self.assertRaises(FileExistsError):
            setup.apply(kernel, store, 1, 1, 1000)
        self.assertEqual(store.active, before)
        self.assertEqual(kernel.writes, writes)

    def test_partial_reservation_does_not_grant_access_and_can_restore(self):
        kernel, store = FakeKernel(), MemoryStore()
        kernel.partial = True
        with self.assertRaisesRegex(setup.SetupError, 'only part'):
            setup.apply(kernel, store, 1, 1, 1000)
        self.assertEqual(kernel.state.hugetlb_shm_group, 0)
        self.assertEqual(store.active['phase'], 'partial')
        self.assertEqual(store.active['observed']['nr_hugepages'], 1287)
        setup.restore(kernel, store)
        self.assertEqual(kernel.state.nr_hugepages, 0)

    def test_failed_group_write_keeps_a_durable_journal(self):
        kernel, store = FakeKernel(), MemoryStore()
        kernel.fail_group = True
        with self.assertRaises(OSError):
            setup.apply(kernel, store, 1, 1, 1000)
        self.assertEqual(store.active['phase'], 'writing_group')
        with self.assertRaisesRegex(setup.SetupError, 'interrupted'):
            setup.restore(kernel, store)

    def test_external_change_is_not_overwritten(self):
        for name, value in (('nr_hugepages', 2000), ('hugetlb_shm_group', 2000)):
            kernel, store = FakeKernel(), MemoryStore()
            setup.apply(kernel, store, 1, 1, 1000)
            kernel.write(name, value)
            writes = list(kernel.writes)
            with self.assertRaisesRegex(setup.SetupError, 'changed externally'):
                setup.restore(kernel, store)
            self.assertEqual(kernel.writes, writes)
            self.assertIsNotNone(store.active)

    def test_restore_refuses_used_reserved_or_surplus_pages(self):
        for changes in ({'HugePages_Free': 1296}, {'HugePages_Rsvd': 1}, {'HugePages_Surp': 1}):
            kernel, store = FakeKernel(), MemoryStore()
            setup.apply(kernel, store, 1, 1, 1000)
            kernel.state.memory.update(changes)
            writes = list(kernel.writes)
            with self.assertRaises(setup.SetupError):
                setup.restore(kernel, store)
            self.assertEqual(kernel.writes, writes)

    def test_expired_boot_archives_without_touching_kernel(self):
        kernel, store = FakeKernel(), MemoryStore()
        setup.apply(kernel, store, 1, 1, 1000)
        kernel.state = replace(kernel.state, boot_id='87654321-4321-4321-4321-cba987654321')
        writes = list(kernel.writes)
        self.assertIn('previous boot', setup.restore(kernel, store))
        self.assertEqual(kernel.writes, writes)
        self.assertEqual(store.history[-1]['phase'], 'expired')

    def test_invalid_backup_cannot_target_other_controls(self):
        kernel, store = FakeKernel(), MemoryStore()
        record = setup.apply(kernel, store, 1, 1, 1000)
        for field, value in (('before', {'nr_hugepages': 0, 'swappiness': 0}),
                             ('observed', {'nr_hugepages': -1, 'hugetlb_shm_group': 1000}),
                             ('phase', 'anything'), ('operation', '../../escape')):
            altered = deepcopy(record)
            altered[field] = value
            with self.assertRaises((setup.SetupError, ValueError)):
                setup.validate_record(altered)

    def test_fake_proc_parser_and_fixed_controls(self):
        with tempfile.TemporaryDirectory(prefix='connectcoin-hugepages-test-') as temporary:
            proc = Path(temporary)
            (proc / 'sys/kernel/random').mkdir(parents=True)
            (proc / 'sys/vm').mkdir()
            (proc / 'sys/kernel/random/boot_id').write_text(BOOT, encoding='ascii')
            for name in setup.CONTROLS:
                (proc / 'sys/vm' / name).write_text('0\n', encoding='ascii')
            text = ''.join(f'{name}: {value}' + ('' if name.startswith('HugePages_') else ' kB') + '\n'
                           for name, value in snapshot().memory.items())
            (proc / 'meminfo').write_text(text, encoding='ascii')
            kernel = setup.Kernel(proc)
            self.assertEqual(kernel.snapshot(), snapshot())
            kernel.write('nr_hugepages', 123)
            self.assertEqual((proc / 'sys/vm/nr_hugepages').read_text(), '123\n')
            for name, value in (('swappiness', 0), ('../../anything', 0), ('nr_hugepages', -1), ('nr_hugepages', True)):
                with self.assertRaises(setup.SetupError):
                    kernel.write(name, value)

    def test_malformed_meminfo_is_rejected(self):
        for text in ('MemTotal: 16 MB\n', 'Hugepagesize: 2048 kB\nHugepagesize: 2048 kB\n', 'MemTotal: invalid kB\n'):
            with self.assertRaises(setup.SetupError):
                setup.meminfo(text)

    def test_backup_permission_symlink_hardlink_guards(self):
        safe = {'st_uid': 0, 'st_mode': stat.S_IFREG | 0o600, 'st_nlink': 1, 'st_size': 100}
        with patch.object(Path, 'lstat', return_value=SimpleNamespace(**safe)):
            setup.StateStore.trusted(Path('fake-file'))
        for changes in ({'st_uid': 1000}, {'st_mode': stat.S_IFREG | 0o666},
                        {'st_mode': stat.S_IFLNK | 0o777}, {'st_nlink': 2}, {'st_size': 70000}):
            with self.subTest(changes), patch.object(Path, 'lstat', return_value=SimpleNamespace(**(safe | changes))):
                with self.assertRaises(setup.SetupError):
                    setup.StateStore.trusted(Path('fake-file'))

    def test_cli_status_never_creates_state_or_writes(self):
        kernel = FakeKernel()
        with patch.object(setup.sys, 'platform', 'linux'), patch.object(setup, 'Kernel', return_value=kernel), \
                patch.object(setup, 'StateStore') as store, redirect_stdout(io.StringIO()) as output:
            self.assertEqual(setup.main(['status']), 0)
        self.assertEqual(json.loads(output.getvalue())['kernel']['nr_hugepages'], 0)
        self.assertFalse(kernel.writes)
        store.assert_not_called()

    def test_cli_mutation_requires_confirmation_and_root(self):
        kernel = FakeKernel()
        with patch.object(setup.sys, 'platform', 'linux'), patch.object(setup, 'Kernel', return_value=kernel), \
                patch.object(setup.os, 'geteuid', return_value=1000, create=True), patch.object(setup, 'StateStore') as store:
            for args in (['restore'], ['restore', '--confirm'], ['apply', '--datasets', '1', '--vm-count', '1', '--group', 'users']):
                with self.assertRaises(setup.SetupError):
                    setup.main(args)
        store.assert_not_called()
        self.assertFalse(kernel.writes)

    @unittest.skipUnless(sys.platform.startswith('linux') and Path('/usr/bin/python3').exists(), 'Linux wrapper test')
    def test_installed_wrapper_ignores_untrusted_python_path(self):
        source = Path(__file__).parent
        with tempfile.TemporaryDirectory(prefix='connectcoin-hugepages-wrapper-') as temporary:
            root = Path(temporary)
            (root / 'bin').mkdir()
            (root / 'share/connectcoin').mkdir(parents=True)
            wrapper = root / 'bin/connectcoin-hugepages'
            shutil.copyfile(source / 'connectcoin-hugepages', wrapper)
            wrapper.chmod(0o755)
            shutil.copyfile(source / 'hugepages.py', root / 'share/connectcoin/hugepages.py')
            hostile = root / 'hostile'
            hostile.mkdir()
            (hostile / 'json.py').write_text('raise RuntimeError("untrusted import")\n', encoding='ascii')
            environment = dict(os.environ, PYTHONPATH=str(hostile), PYTHONHOME=str(hostile))
            result = subprocess.run([str(wrapper), '--help'], cwd=hostile, env=environment,
                                    capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('restore', result.stdout)


if __name__ == '__main__':
    unittest.main()
