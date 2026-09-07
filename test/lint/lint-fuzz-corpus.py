# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check installation of project-owned fuzz seeds without running a node."""

import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import runpy
import subprocess
import tempfile
from unittest.mock import patch


def main():
    root = Path(__file__).resolve().parents[2]
    runner = runpy.run_path(str(root / 'test/fuzz/test_runner.py'))
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
            result = subprocess.CompletedProcess([], 0, stderr='#8 DONE\n')
            with patch('subprocess.run', return_value=result) as run, ThreadPoolExecutor(max_workers=1) as pool:
                runner['run_once'](
                    fuzz_pool=pool, corpus=corpus, test_list=['p2c_tls_proof'], src_dir=root,
                    fuzz_bin='unused-fuzz-binary', using_libfuzzer=libfuzzer, use_valgrind=False,
                    empty_min_time=60, corpus_shards=1, corpus_shard_min_files=750,
                    seeded_empty_targets=seeded,
                )
                args = run.call_args.args[0]
                assert ('-max_total_time=60' in args) == libfuzzer
                assert target in args
                assert '-runs=1' not in args
    print(f'Bundled P2C fuzz corpus: {len(expected)} inputs; installation and idempotence valid')


if __name__ == '__main__':
    main()
