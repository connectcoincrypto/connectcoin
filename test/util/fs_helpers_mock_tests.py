# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Compile the current descriptor-limit function with real utility headers.

Run with a GCC-compatible compiler: python3 test/util/fs_helpers_mock_tests.py
--cxx g++. This exercises simulated OS calls, not native resource limits, and
does not replace compiling fs_helpers.cpp on each supported platform.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/util/fs_helpers.cpp').read_text(encoding='utf-8')
    start = 'int RaiseFileDescriptorLimit()\n{'
    end = '\n}\n'
    assert source.count(start) == 1, 'Review the function extraction boundary'
    begin = source.index(start)
    finish = source.index(end, begin) + len(end)
    function = source[begin:finish]
    harness = Path(__file__).with_suffix('.cpp').read_text(encoding='utf-8')
    assert harness.count('/* FUNCTION_UNDER_TEST */') == 1
    with tempfile.TemporaryDirectory(prefix='connectcoin-fd-tests-') as directory:
        work = Path(directory)
        unit = work / 'fs_helpers_mock.cpp'
        executable = work / ('fs_helpers_mock.exe' if os.name == 'nt' else 'fs_helpers_mock')
        unit.write_text(harness.replace('/* FUNCTION_UNDER_TEST */', function), encoding='utf-8')
        command = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                   '-I', str(root / 'src'), str(unit), '-o', str(executable)]
        if os.name == 'nt':
            command.append('-static')
        for variant in ('POSIX_UNSIGNED', 'POSIX_SIGNED', 'DARWIN_UNSIGNED', 'DARWIN_SIGNED', 'CRT_UCRT', 'CRT_LEGACY'):
            subprocess.run([*command, f'-DTEST_{variant}'], check=True, timeout=60)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=5)
            print(f'{variant}: {result.stdout.strip()}', flush=True)

        # The real util/string.h must reject the namespace mistake that escaped
        # earlier tests whose dependency stub incorrectly defined ToString in
        # the global namespace. Keep this check free of any ToString stub.
        assert 'util::ToString(' in function, 'Review the formatter regression check'
        broken = function.replace('util::ToString(', '::ToString(')
        unit.write_text(harness.replace('/* FUNCTION_UNDER_TEST */', broken), encoding='utf-8')
        result = subprocess.run([*command, '-DTEST_POSIX_UNSIGNED'], capture_output=True, text=True, timeout=60)
        assert result.returncode != 0 and 'ToString' in result.stderr, result.stderr
        print('Namespace regression: ::ToString correctly fails compilation with the real header')


if __name__ == '__main__':
    main()
