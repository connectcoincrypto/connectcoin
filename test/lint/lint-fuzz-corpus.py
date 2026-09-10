# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check fuzz engine detection and corpus replay without running a node."""

import ast
import hashlib
import json
import math
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile
from unittest import TestCase
from unittest.mock import patch


def check_shared_cache_writer(root):
    """Evaluate the actual small workflow predicate over every relevant context."""
    source = (root / '.github/actions/cache/save/action.yml').read_text(encoding='utf-8')
    step = source.split('- name: Save Ccache cache\n', 1)[1].split('\n    - name:', 1)[0]
    condition = next(line.strip().removeprefix('if: ${{ ').removesuffix(' }}')
                     for line in step.splitlines() if line.strip().startswith('if:'))
    tree = ast.parse(condition.replace('&&', ' and ').replace('||', ' or '), mode='eval')

    def evaluate(node, values):
        if isinstance(node, ast.Expression):
            return evaluate(node.body, values)
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Attribute):
            return values[ast.unparse(node)]
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and not node.args and not node.keywords:
            assert node.func.id in ('success', 'failure')
            return values['status'] == node.func.id
        if isinstance(node, ast.BoolOp):
            operation = {ast.And: all, ast.Or: any}[type(node.op)]
            return operation(evaluate(child, values) for child in node.values)
        if isinstance(node, ast.Compare) and len(node.ops) == 1 and isinstance(node.ops[0], ast.Eq):
            return evaluate(node.left, values) == evaluate(node.comparators[0], values)
        raise AssertionError(f'Review new cache predicate syntax: {ast.dump(node)}')

    for event in ('push', 'pull_request', 'workflow_dispatch'):
        for provider in ('gha', 'warp'):
            for branch in ('main', 'feature'):
                for count in ('', '1', '4'):
                    for index in ('', '0', '1', '2', '3'):
                        values = {
                            'github.event_name': event, 'inputs.provider': provider,
                            'github.ref_name': branch, 'github.event.repository.default_branch': 'main',
                            'env.FUZZ_SHARD_COUNT': count, 'env.FUZZ_SHARD_INDEX': index,
                        }
                        for status in ('success', 'failure', 'cancelled'):
                            values['status'] = status
                            expected = status != 'cancelled' and event == 'push' and (provider == 'gha' or branch == 'main') and (count == '' or index == '0')
                            assert evaluate(tree, values) == expected, values


def check_engine_detection(runner, root):
    detect = runner['detect_fuzz_engine']
    arguments = dict(fuzz_bin='unused-fuzz-binary', target='rpc', source_dir=root)
    assertions = TestCase()
    for help_text, libfuzzer in (
        ('libFuzzer: coverage-guided fuzzing\n', True),
        ('ConnectCoin fuzz: corpus replay\n', False),
        ('Warning: FUZZ_NONDETERMINISM env var set\nConnectCoin fuzz: corpus replay\n', False),
    ):
        result = subprocess.CompletedProcess([], 0, stdout='', stderr=help_text)
        with patch('subprocess.run', return_value=result) as run:
            assert detect(**arguments) == libfuzzer
            assert run.call_args.args[0] == ['unused-fuzz-binary', '-help=1']
            assert run.call_args.kwargs['env']['FUZZ'] == 'rpc'
            assert run.call_args.kwargs['timeout'] == 300
            assert run.call_args.kwargs['stdin'] == subprocess.DEVNULL
    for exit_code in (-6, 1, 3):
        # Even a crash which mentions libFuzzer must fail, not select an engine.
        result = subprocess.CompletedProcess([], exit_code, stdout='', stderr='libFuzzer: RPC initialization failed')
        with patch('subprocess.run', return_value=result), assertions.assertRaises(subprocess.CalledProcessError):
            detect(**arguments)
    for help_text in ('', 'unexpected output'):
        result = subprocess.CompletedProcess([], 0, stdout='', stderr=help_text)
        with patch('subprocess.run', return_value=result), assertions.assertRaises(RuntimeError):
            detect(**arguments)
    with patch('subprocess.run', side_effect=subprocess.TimeoutExpired('fuzz', 300)), assertions.assertRaises(subprocess.TimeoutExpired):
        detect(**arguments)


def check_active_timing_configurations(runner, root, profile_path):
    """Reject stale profiles requested by CI before any expensive build starts."""
    committed = json.loads(profile_path.read_text(encoding='utf-8'))
    current_commit = (root / 'ci/qa-assets-commit.txt').read_text(encoding='utf-8').strip()
    configuration_files = set(committed['configurations']) | {
        path.relative_to(root).as_posix() for path in (root / 'ci/test').glob('00_setup_env*.sh')
    }
    active = set()
    for config_file in sorted(configuration_files):
        config_path = root / config_file
        config_text = config_path.read_text(encoding='utf-8') if config_path.exists() else ''
        tokens = shlex.split(config_text, comments=True)
        requests_profile = any('--timings-profile' in token for token in tokens)
        requests_config = any('--timings-config' in token for token in tokens)
        assert requests_profile == requests_config, f'Remove or configure both timing arguments in {config_file}'
        data = committed['configurations'].get(config_file)
        if requests_profile:
            assert data is not None, f'Missing timings configuration for {config_file}'
            result = subprocess.CompletedProcess([], 0, stdout=current_commit, stderr='')
            with patch('subprocess.run', return_value=result):
                loaded = runner['load_fuzz_timings'](
                    profile_path=profile_path, config_file=config_file, source_dir=root, corpus_dir=root,
                    using_libfuzzer=data['engine'] == 'libfuzzer', required=True,
                )
            assert loaded == data['target_seconds'] and loaded
            active.add(config_file)
        if data is not None:
            assert data['engine'] in ('libfuzzer', 'replay')
            assert data['target_seconds'] and all(
                type(value) in (int, float) and math.isfinite(value) and value > 0
                for value in data['target_seconds'].values()
            )
    return active


def check_timing_profiles(runner, root):
    load = runner['load_fuzz_timings']
    select = runner['select_fuzz_shard']
    commit = 'a' * 40
    result = subprocess.CompletedProcess([], 0, stdout=commit + '\n', stderr='')
    with tempfile.TemporaryDirectory(prefix='connectcoin-fuzz-timings-') as directory:
        source = Path(directory)
        configuration = source / 'config.sh'
        configuration.write_text('export SANITIZERS=memory\n', encoding='utf-8')
        record = {
            'config_sha256': hashlib.sha256(configuration.read_text(encoding='utf-8').encode()).hexdigest(),
            'engine': 'replay', 'target_seconds': {'slow': 300, 'fast': 0.1},
        }
        profile = {'version': 1, 'corpus_commit': commit, 'configurations': {'config.sh': record}}
        profile_path = source / 'timings.json'
        arguments = dict(profile_path=profile_path, config_file='config.sh', source_dir=source,
                         corpus_dir=source, using_libfuzzer=False)

        def attempt(candidate, **overrides):
            profile_path.write_text(json.dumps(candidate), encoding='utf-8')
            with patch('subprocess.run', return_value=result):
                return load(**(arguments | overrides))

        assert attempt(profile) == record['target_seconds']
        assert attempt(profile, required=True) == record['target_seconds']
        assert load(**(arguments | {'profile_path': None})) == {}
        assert attempt(profile, config_file='unknown.sh') == {}
        assert attempt(profile, using_libfuzzer=True) == {}
        for field, value in (('version', 2), ('version', True), ('corpus_commit', 'b' * 40)):
            assert attempt(profile | {field: value}) == {}
        for field, value in (('config_sha256', '0' * 64), ('engine', 'unknown')):
            assert attempt(profile | {'configurations': {'config.sh': record | {field: value}}}) == {}
        for values in ({}, [], None, {'bad': True}, {'bad': 0}, {'bad': -1}, {'bad': '2'},
                       {'bad': float('nan')}, {'bad': float('inf')}, {'bad': float('-inf')},
                       {'one': 1e308, 'two': 1e308}, {'': 1}):
            invalid = profile | {'configurations': {'config.sh': record | {'target_seconds': values}}}
            assert attempt(invalid) == {}, values
        for candidate in ([], {}, None, {'version': 1, 'configurations': []}):
            assert attempt(candidate) == {}
        assert attempt(profile) == record['target_seconds']
        for error in (OSError('git missing'), subprocess.TimeoutExpired('git', 30),
                      subprocess.CalledProcessError(1, 'git')):
            with patch('subprocess.run', side_effect=error):
                assert load(**arguments) == {}
            # A transient error on just one shard must not silently select a
            # different partition than the other shards using the profile.
            with patch('subprocess.run', side_effect=error), TestCase().assertRaisesRegex(ValueError, 'multi-shard'):
                load(**(arguments | {'required': True}))
        with TestCase().assertRaises(ValueError):
            attempt(profile, config_file='unknown.sh', required=True)
        with TestCase().assertRaises(ValueError):
            load(**(arguments | {'profile_path': None, 'required': True}))
        profile_path.write_text('{invalid JSON', encoding='utf-8')
        assert load(**arguments) == {}
        configuration.write_text('export SANITIZERS=address\n', encoding='utf-8')
        assert attempt(profile) == {}

        targets = ['slow', 'fast', 'new', 'empty', 'medium']
        expected_inputs = set()
        for index, target in enumerate(targets):
            corpus = source / target
            corpus.mkdir()
            for input_index in range(index):
                data = f'{target}:{input_index}'.encode()
                (corpus / str(input_index)).write_bytes(data)
                expected_inputs.add(data)
        for count in (1, 2, 4, 8):
            for timings in (None, {}, {'unknown': 5}, record['target_seconds']):
                shards = []
                for index in range(count):
                    selection = dict(corpus_dir=source, shard_count=count, shard_index=index, timings=timings)
                    shard, loads = select(targets=targets, **selection)
                    assert (shard, loads) == select(targets=list(reversed(targets)), **selection)
                    shards.append(shard)
                flattened = [target for shard in shards for target in shard]
                assert len(flattened) == len(targets)
                assert sorted(flattened) == sorted(targets)  # union, disjointness, and unknown targets
        ordered, _ = select(targets=targets, corpus_dir=source, shard_count=1, shard_index=0,
                            timings=record['target_seconds'])
        assert ordered.index('slow') < ordered.index('fast')
        with TestCase().assertRaises(ValueError):
            select(targets=targets, corpus_dir=source, shard_count=4, shard_index=0,
                   timings={'slow': 1e308})

        # A timings-ordered run still consumes every corpus input exactly once,
        # including targets absent from the profile and partitioned corpora.
        seen_inputs = []

        def replay(args, **_kwargs):
            assert len(args) == 2  # Native replay arguments remain unchanged.
            seen_inputs.extend(path.read_bytes() for path in Path(args[1]).iterdir())
            return subprocess.CompletedProcess(args, 0, stderr='')

        with patch('subprocess.run', side_effect=replay), ThreadPoolExecutor(max_workers=2) as pool:
            runner['run_once'](
                fuzz_pool=pool, corpus=source, test_list=ordered, src_dir=root,
                fuzz_bin='unused-fuzz-binary', using_libfuzzer=False, use_valgrind=False,
                empty_min_time=60, corpus_shards=3, corpus_shard_min_files=1, seeded_empty_targets=set(),
            )
        assert len(seen_inputs) == len(expected_inputs)
        assert set(seen_inputs) == expected_inputs

        # The lint must fail as early as runtime would when a still-enabled
        # profile becomes stale, even if only a setup comment changed.
        (source / 'ci').mkdir()
        corpus_pin = source / 'ci/qa-assets-commit.txt'
        corpus_pin.write_text(commit + '\n', encoding='utf-8')
        enabled_config = 'export FUZZ_TESTS_CONFIG="--timings-profile=timings.json --timings-config=config.sh"\n'
        configuration.write_text(enabled_config, encoding='utf-8')
        record['config_sha256'] = hashlib.sha256(enabled_config.encode()).hexdigest()
        profile_path.write_text(json.dumps(profile), encoding='utf-8')
        assert check_active_timing_configurations(runner, source, profile_path) == {'config.sh'}
        configuration.write_text(enabled_config + '# Changed comment\n', encoding='utf-8')
        with TestCase().assertRaisesRegex(ValueError, 'configuration changed'):
            check_active_timing_configurations(runner, source, profile_path)
        configuration.write_text(enabled_config, encoding='utf-8')
        corpus_pin.write_text('b' * 40 + '\n', encoding='utf-8')
        with TestCase().assertRaisesRegex(ValueError, 'corpus commit changed'):
            check_active_timing_configurations(runner, source, profile_path)
        configuration.write_text('export FUZZ_TESTS_CONFIG="--timings-config=config.sh"\n', encoding='utf-8')
        with TestCase().assertRaisesRegex(AssertionError, 'both timing arguments'):
            check_active_timing_configurations(runner, source, profile_path)
        # Removing both arguments deliberately restores the original estimator
        # on all shards. Old profile entries may then remain as unused history.
        configuration.write_text('# Removed --timings-profile and --timings-config\nexport FUZZ_TESTS_CONFIG=""\n', encoding='utf-8')
        assert check_active_timing_configurations(runner, source, profile_path) == set()

    check_active_timing_configurations(runner, root, root / 'ci/fuzz-timings.json')
    print('Fuzz timing profiles: version/configuration/engine/corpus guards and complete replay valid')


def main():
    root = Path(__file__).resolve().parents[2]
    check_shared_cache_writer(root)
    runner = runpy.run_path(str(root / 'test/fuzz/test_runner.py'))
    check_engine_detection(runner, root)
    check_timing_profiles(runner, root)
    install = runner['install_p2c_seed_corpus']
    seeds = json.loads((root / 'src/test/data/p2c_fuzz_seeds.json').read_text(encoding='utf-8'))
    expected = {bytes.fromhex(seed['hex']) for seed in seeds}
    assert len(expected) == len(seeds) and expected
    with tempfile.TemporaryDirectory(prefix='connectcoin-fuzz-seeds-') as directory:
        corpus = Path(directory)
        assert install(targets=['other_target'], corpus_dir=corpus, source_dir=root) == set()
        assert not list(corpus.iterdir())
        seeded = install(targets=['p2c_tls_proof'], corpus_dir=corpus, source_dir=root)
        assert seeded == {'p2c_tls_proof'}
        target = corpus / 'p2c_tls_proof'
        assert {path.read_bytes() for path in target.iterdir()} == expected
        for path in target.iterdir():
            assert path.name == 'connectcoin-' + hashlib.sha256(path.read_bytes()).hexdigest()
        (target / 'external-seed').write_bytes(b'preserve this input')
        assert install(targets=['p2c_tls_proof'], corpus_dir=corpus, source_dir=root) == set()
        assert (target / 'external-seed').read_bytes() == b'preserve this input'
        assert len(list(target.iterdir())) == len(expected) + 1
        # Seeding must not replace the existing Unix CI mutation budget with
        # one-pass replay. Windows without libFuzzer must still replay files.
        for libfuzzer in (True, False):
            for seeded_empty in (seeded, set()):
                result = subprocess.CompletedProcess([], 0, stderr='#8 DONE\n')
                with patch('subprocess.run', return_value=result) as run, ThreadPoolExecutor(max_workers=1) as pool:
                    runner['run_once'](
                        fuzz_pool=pool, corpus=corpus, test_list=['p2c_tls_proof'], src_dir=root,
                        fuzz_bin='unused-fuzz-binary', using_libfuzzer=libfuzzer, use_valgrind=False,
                        empty_min_time=60, corpus_shards=1, corpus_shard_min_files=750,
                        seeded_empty_targets=seeded_empty,
                    )
                    args = run.call_args.args[0]
                    assert ('-max_total_time=60' in args) == (libfuzzer and bool(seeded_empty))
                    assert target in args
                    # Populated libFuzzer corpora must replay once, never mutate indefinitely.
                    assert ('-runs=1' in args) == (libfuzzer and not seeded_empty)
    print(f'Bundled P2C fuzz corpus: {len(expected)} inputs; installation and idempotence valid')


if __name__ == '__main__':
    main()
