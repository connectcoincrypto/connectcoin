# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Explicit, temporary Linux HugeTLB setup; never invoked by package installation."""

import argparse
from contextlib import contextmanager
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import uuid


class SetupError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SetupError(message)


# src/randomx/src/configuration.h: round EACH allocation to the kernel page size.
DATASET_BYTES = 2147483648 + 33554368
CACHE_BYTES = 256 * 1024 * 1024
SCRATCHPAD_BYTES = 2 * 1024 * 1024
STATE_DIRECTORY = Path('/var/lib/connectcoin-hugepages')
CONTROLS = ('nr_hugepages', 'hugetlb_shm_group')


def number(text):
    require(re.fullmatch(r'[0-9]+\s*', text) is not None, 'Invalid kernel integer')
    value = int(text)
    require(value <= 2**63 - 1, 'Kernel integer is out of range')
    return value


def meminfo(text):
    wanted = {'MemTotal', 'MemAvailable', 'Hugepagesize', 'Hugetlb',
              'HugePages_Total', 'HugePages_Free', 'HugePages_Rsvd', 'HugePages_Surp'}
    result = {}
    for line in text.splitlines():
        name, separator, value = line.partition(':')
        if not separator or name not in wanted:
            continue
        require(name not in result, f'Duplicate /proc/meminfo field: {name}')
        if not name.startswith('HugePages_'):
            require(value.strip().endswith(' kB'), f'Expected kB for {name}')
            value = value.strip()[:-3]
        result[name] = number(value.strip())
    require(result.keys() == wanted, 'Missing Linux HugeTLB or MemAvailable information')
    return result


@dataclass(frozen=True)
class Snapshot:
    boot_id: str
    nr_hugepages: int
    hugetlb_shm_group: int
    memory: dict

    def controls(self):
        return {name: getattr(self, name) for name in CONTROLS}


class Kernel:
    # Dependency injection is for offline tests only; the CLI has no path override.
    def __init__(self, proc=Path('/proc')):
        self.proc = proc

    def snapshot(self):
        boot_id = str(uuid.UUID((self.proc / 'sys/kernel/random/boot_id').read_text().strip()))
        values = {name: number((self.proc / 'sys/vm' / name).read_text()) for name in CONTROLS}
        return Snapshot(boot_id, **values, memory=meminfo((self.proc / 'meminfo').read_text()))

    def write(self, name, value):
        require(name in CONTROLS and type(value) is int and 0 <= value <= 2**32 - 1,
                'Invalid HugeTLB control write')
        with (self.proc / 'sys/vm' / name).open('w', encoding='ascii') as stream:
            stream.write(f'{value}\n')


def validate_snapshot(snapshot):
    memory = snapshot.memory
    require(memory['Hugepagesize'] == 2048, 'Only the default 2 MiB HugeTLB page size is supported; no settings changed')
    require(memory['HugePages_Surp'] == 0, 'Surplus huge pages are present; stop their users before configuring')
    require(memory['HugePages_Total'] == snapshot.nr_hugepages,
            'HugeTLB counters changed while reading; retry after stopping their users')
    require(0 <= memory['HugePages_Rsvd'] <= memory['HugePages_Free'] <= snapshot.nr_hugepages,
            'Inconsistent HugeTLB counters; retry')
    require(0 < memory['MemAvailable'] <= memory['MemTotal'], 'Invalid available-memory information')
    require(memory['Hugetlb'] >= snapshot.nr_hugepages * memory['Hugepagesize'], 'Invalid total HugeTLB memory')


def plan(snapshot, datasets, vm_count, gid):
    require(type(datasets) is int and datasets in (1, 2), 'Choose one or two simultaneous datasets')
    require(type(vm_count) is int and 1 <= vm_count <= 1024, 'VM count must be between 1 and 1024')
    require(type(gid) is int and 0 < gid <= 2**32 - 2, 'Choose an existing non-root group')
    validate_snapshot(snapshot)
    require(snapshot.hugetlb_shm_group in (0, gid),
            'Another non-root HugeTLB group is configured; this helper will not replace its access')
    page_bytes = snapshot.memory['Hugepagesize'] * 1024
    def pages(size):
        return (size + page_bytes - 1) // page_bytes
    # While FAST initializes, hashing can retain the separate shared LIGHT
    # context. Its cache coexists with the FAST initialization cache(s).
    requested = datasets * (pages(DATASET_BYTES) + pages(CACHE_BYTES)) + pages(CACHE_BYTES) + vm_count * pages(SCRATCHPAD_BYTES)
    free_unreserved = snapshot.memory['HugePages_Free'] - snapshot.memory['HugePages_Rsvd']
    additional = max(0, requested - free_unreserved)
    target = snapshot.nr_hugepages + additional
    memory_kb = additional * snapshot.memory['Hugepagesize']
    # A pool is unswappable and unavailable to normal applications. Keep both
    # an absolute 2 GiB floor and at least a quarter of host RAM available.
    headroom_kb = max(2 * 1024 * 1024, snapshot.memory['MemTotal'] // 4)
    require(snapshot.memory['MemAvailable'] - memory_kb >= headroom_kb,
            'Insufficient MemAvailable: reservation must leave at least 2 GiB and 25% of total RAM')
    require(additional == 0 or snapshot.memory['Hugetlb'] + memory_kb <= snapshot.memory['MemTotal'] // 2,
            'Refusing to reserve more than 50% of total RAM as HugeTLB memory')
    return {'datasets': datasets, 'vm_count': vm_count, 'pages_budget': requested,
            'additional_pages': additional, 'additional_kib': memory_kb,
            'target': {'nr_hugepages': target, 'hugetlb_shm_group': gid},
            'note': 'Shared pool; not a guarantee of allocation, NUMA locality, or all VM pages. No reboot persistence.'}


class StateStore:
    def __init__(self, directory=STATE_DIRECTORY):
        self.directory = directory
        self.path = directory / 'active.json'

    @staticmethod
    def trusted(path, directory=False):
        info = path.lstat()
        require(info.st_uid == 0 and not info.st_mode & 0o022,
                f'Refusing non-root-owned or writable state path: {path}')
        require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
                f'Refusing symlink or unexpected state path: {path}')
        if not directory:
            require(info.st_nlink == 1 and info.st_size <= 65536 and not info.st_mode & 0o077,
                    f'Unsafe state file (requires private root ownership): {path}')

    @contextmanager
    def locked(self):
        import fcntl  # Linux only; offline tests also run on Windows.
        for parent in self.directory.parents:
            self.trusted(parent, directory=True)
        self.directory.mkdir(mode=0o700, exist_ok=True)
        self.trusted(self.directory, directory=True)
        require(not self.directory.lstat().st_mode & 0o077, 'State directory must have mode 0700')
        descriptor = os.open(self.directory / 'lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            self.trusted(self.directory / 'lock')
            require(os.fstat(descriptor).st_ino == (self.directory / 'lock').lstat().st_ino, 'State lock changed')
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise SetupError('Another Huge Pages setup operation is active') from error
            yield
        finally:
            os.close(descriptor)

    def load(self):
        self.trusted(self.path)
        with self.path.open(encoding='utf-8') as stream:
            record = json.load(stream)
        validate_record(record)
        return record

    def save(self, record, create=False):
        validate_record(record)
        payload = json.dumps(record, indent=2) + '\n'
        if create:
            descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
        else:
            self.trusted(self.path)
            descriptor, name = tempfile.mkstemp(prefix='journal-', suffix='.json', dir=self.directory)
            temporary = Path(name)
            try:
                with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
                    stream.write(payload)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, self.path)
            finally:
                temporary.unlink(missing_ok=True)
        self.sync_directory()

    def sync_directory(self):
        descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def archive(self, record, phase):
        record['phase'] = phase
        self.save(record)
        destination = self.directory / f'{phase}-{record["operation"]}.json'
        # link() refuses an existing destination instead of replacing a backup.
        os.link(self.path, destination)
        self.path.unlink()
        self.sync_directory()


PHASES = {'prepared', 'writing_pages', 'pages_written', 'partial', 'writing_group', 'applied',
          'restoring_group', 'restore_group_done', 'restoring_pages', 'restore_partial', 'restored', 'expired'}


def validate_record(record):
    require(isinstance(record, dict) and record.get('schema') == 1, 'Invalid backup schema')
    for name in ('operation', 'boot_id'):
        require(str(uuid.UUID(record[name])) == record[name], f'Invalid backup {name}')
    require(record.get('phase') in PHASES, 'Invalid backup phase')
    for field in ('before', 'target', 'observed'):
        values = record.get(field)
        require(isinstance(values, dict) and values.keys() == set(CONTROLS), 'Invalid backup controls')
        require(all(type(value) is int and 0 <= value <= 2**32 - 1 for value in values.values()),
                'Invalid backup control values')
    require(record['before']['nr_hugepages'] <= record['observed']['nr_hugepages'] <= record['target']['nr_hugepages'],
            'Backup page counts are inconsistent')
    require(record['before']['hugetlb_shm_group'] in (0, record['target']['hugetlb_shm_group']) and
            record['observed']['hugetlb_shm_group'] in (record['before']['hugetlb_shm_group'], record['target']['hugetlb_shm_group']),
            'Backup group values are inconsistent')


def unchanged(kernel, record, idle=False):
    snapshot = kernel.snapshot()
    require(snapshot.boot_id == record['boot_id'], 'Boot changed; no settings restored')
    require(snapshot.controls() == record['observed'], 'HugeTLB settings changed externally; refusing to overwrite them')
    validate_snapshot(snapshot)
    if idle:
        require(snapshot.memory['HugePages_Free'] == snapshot.nr_hugepages and snapshot.memory['HugePages_Rsvd'] == 0,
                'Huge pages are in use or reserved; stop Core and every other HugeTLB user before restore')
    return snapshot


def apply(kernel, store, datasets, vm_count, gid):
    snapshot = kernel.snapshot()
    proposal = plan(snapshot, datasets, vm_count, gid)
    record = {'schema': 1, 'operation': str(uuid.uuid4()), 'boot_id': snapshot.boot_id,
              'before': snapshot.controls(), 'target': proposal['target'], 'observed': snapshot.controls(),
              'phase': 'prepared', 'original_snapshot': asdict(snapshot), 'plan': proposal}
    # An outstanding backup is never replaced. Each phase is durable BEFORE
    # its corresponding kernel write; interruption in a write requires review.
    store.save(record, create=True)
    unchanged(kernel, record)
    if proposal['additional_pages']:
        record['phase'] = 'writing_pages'
        store.save(record)
        kernel.write('nr_hugepages', record['target']['nr_hugepages'])
        after = kernel.snapshot()
        require(after.hugetlb_shm_group == record['observed']['hugetlb_shm_group'], 'Group changed during allocation; preserve backup for administrator review')
        record['observed']['nr_hugepages'] = after.nr_hugepages
        record['phase'] = 'pages_written' if after.nr_hugepages == record['target']['nr_hugepages'] else 'partial'
        store.save(record)
        require(record['phase'] == 'pages_written', 'Kernel allocated only part of the requested pool. No group access was changed. Run restore to undo the recorded partial reservation')
    unchanged(kernel, record)
    if record['observed']['hugetlb_shm_group'] != gid:
        record['phase'] = 'writing_group'
        store.save(record)
        kernel.write('hugetlb_shm_group', gid)
        after = kernel.snapshot()
        require(after.controls() == record['target'], 'Settings changed during group update; preserve backup for administrator review')
        record['observed'] = after.controls()
    record['phase'] = 'applied'
    store.save(record)
    return record


def restore(kernel, store):
    record = store.load()
    snapshot = kernel.snapshot()
    if snapshot.boot_id != record['boot_id']:
        store.archive(record, 'expired')
        return 'Backup belongs to a previous boot; archived without changing this boot\'s settings.'
    require(record['phase'] not in {'writing_pages', 'writing_group', 'restoring_group', 'restoring_pages'},
            'An operation was interrupted during a write. Backup retained; administrator review is required, no automatic overwrite')
    unchanged(kernel, record, idle=True)
    for name, pending, completed in (('hugetlb_shm_group', 'restoring_group', 'restore_group_done'),
                                     ('nr_hugepages', 'restoring_pages', 'restore_partial')):
        if record['observed'][name] == record['before'][name]:
            continue
        unchanged(kernel, record, idle=True)
        record['phase'] = pending
        store.save(record)
        kernel.write(name, record['before'][name])
        after = kernel.snapshot()
        other = next(control for control in CONTROLS if control != name)
        require(after.controls()[other] == record['observed'][other], 'Settings changed during restore; backup retained')
        record['observed'][name] = after.controls()[name]
        record['phase'] = completed
        store.save(record)
        require(record['observed'][name] == record['before'][name], 'Kernel did not fully restore the value; backup retained, retry after stopping HugeTLB users')
    unchanged(kernel, record, idle=True)
    store.archive(record, 'restored')
    return 'Original runtime settings restored; audit backup retained. No files in sysctl.d were changed.'


def group_id(name):
    import grp
    require(re.fullmatch(r'[A-Za-z0-9_.][A-Za-z0-9_.-]{0,63}', name) is not None, 'Invalid group name')
    try:
        return grp.getgrnam(name).gr_gid
    except KeyError as error:
        raise SetupError('Group does not exist; this helper never creates groups or adds members') from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog='No automatic privilege elevation, mining, reboot persistence, THP changes, or group membership changes.')
    commands = parser.add_subparsers(dest='command', required=True)
    status_parser = commands.add_parser('status', help='Read kernel state only; an optional group shows a reservation plan')
    apply_parser = commands.add_parser('apply', help='Explicitly reserve runtime pages and permit one existing group')
    for child in (status_parser, apply_parser):
        child.add_argument('--datasets', type=int, choices=(1, 2), required=child is apply_parser, default=1)
        child.add_argument('--vm-count', type=int, required=child is apply_parser, default=1,
                           help='Budgeted concurrent VM scratchpads (mining AND validation), 1..1024')
        child.add_argument('--group', required=child is apply_parser, help='Existing non-root group; grants access to every member')
    restore_parser = commands.add_parser('restore', help='Restore this helper\'s saved runtime settings only when safely idle')
    for child in (apply_parser, restore_parser):
        child.add_argument('--confirm', action='store_true', help='Confirm the privileged changes described in the documentation')
    options = parser.parse_args(argv)
    require(sys.platform.startswith('linux'), 'This helper only configures Linux; no settings changed')
    kernel = Kernel()
    if options.command == 'status':
        result = {'kernel': asdict(kernel.snapshot()), 'note': 'Read-only. Runtime HugeTLB pool, not transparent huge pages. Membership in hugetlb_shm_group or CAP_IPC_LOCK is required for unprivileged MAP_HUGETLB.'}
        if options.group:
            result['plan'] = plan(kernel.snapshot(), options.datasets, options.vm_count, group_id(options.group))
        print(json.dumps(result, indent=2))
        return 0
    require(options.confirm, 'No changes made: explicitly pass --confirm after reviewing status and the documentation')
    require(os.geteuid() == 0, 'Administrator privileges required; invoke this helper explicitly using sudo (never run Core as root)')
    store = StateStore()
    with store.locked():
        if options.command == 'apply':
            record = apply(kernel, store, options.datasets, options.vm_count, group_id(options.group))
            print(json.dumps(record, indent=2))
            print('Applied for this boot only. Run Core as a normal member of the selected group. Restart Core normally to retry dataset allocation; this tool does not start it.')
        else:
            print(restore(kernel, store))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (SetupError, OSError, ValueError, KeyError) as error:
        print(f'Huge Pages setup: {error}. Any existing backup remains in {STATE_DIRECTORY}.', file=sys.stderr)
        sys.exit(1)
