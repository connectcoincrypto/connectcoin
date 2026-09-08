# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check fuzz engine detection and corpus replay without running a node."""

import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import runpy
import subprocess
import tempfile
from unittest import TestCase
from unittest.mock import patch


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


def main():
    root = Path(__file__).resolve().parents[2]
    runner = runpy.run_path(str(root / 'test/fuzz/test_runner.py'))
    check_engine_detection(runner, root)
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
