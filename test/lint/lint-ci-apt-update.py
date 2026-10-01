# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise fail-closed CI APT updates without package installs or network access."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HARNESS = r'''
set -euo pipefail
EVENTS="$(pwd -P)/events.log"
FAIL_UPDATES=$1
export EVENTS FAIL_UPDATES
APT_LLVM_V=22 CI_OS_NAME=linux APPEND_APT_SOURCES_LIST=
PACKAGES=compiler CI_BASE_PACKAGES=base CONTAINER_NAME=ci_native_asan
CI_RETRY_EXE=run_retry
# Match APT's default partial-index behavior: without strict mode an update
# reports success and installation would continue using stale indexes.
apt-get() {
  local strict=0
  if [[ ${1:-} == -o && ${2:-} == APT::Update::Error-Mode=any ]]; then
    strict=1
    shift 2
  fi
  case ${1:-} in
    update)
      [[ $# == 1 ]] || exit 91
      printf 'update %s %s\n' "${RETRY_ATTEMPT:-0}" "$strict" >> "$EVENTS"
      if (( ${RETRY_ATTEMPT:-0} < FAIL_UPDATES )); then
        if (( strict )); then return 100; fi
      fi
      ;;
    install) printf 'install\n' >> "$EVENTS" ;;
    *) exit 92 ;;
  esac
}
sleep() { :; }
export -f apt-get sleep
# Ensure the extracted package branch chooses APT even on an Alpine host.
command() {
  if [[ $# == 2 && $1 == -v && $2 == apk ]]; then return 1; fi
  builtin command "$@"
}
'''


def apt_sections(root):
    source = (root / 'ci/test/01_base_install.sh').read_text(encoding='utf-8')
    bootstrap = 'if [ -n "${APT_LLVM_V}" ]; then'
    download_key = '  ${CI_RETRY_EXE} curl --fail --location'
    packages = 'if command -v apk >/dev/null 2>&1; then'
    next_section = 'if [[ ${HOST:-} == x86_64-w64-mingw32* ]]; then'
    assert source.count(download_key) == source.count(packages) == source.count(next_section) == 1
    assert source.index(bootstrap) < source.index(download_key) < source.index(packages) < source.index(next_section)
    runtime = (root / 'ci/test/03_test_script.sh').read_text(encoding='utf-8')
    refresh = 'if [[ "$CONTAINER_NAME" == "ci_native_asan" ]]; then'
    host = '# What host to compile for.'
    assert runtime.count(refresh) == runtime.count(host) == 1
    assert runtime.index(refresh) < runtime.index(host)
    return {
        'llvm-bootstrap': source[source.index(bootstrap):source.index(download_key)] + 'fi\n',
        'package-indexes': source[source.index(packages):source.index(next_section)],
        'runtime-asan': runtime[runtime.index(refresh):runtime.index(host)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bash', default=os.environ.get('BASH', 'bash'), help='Bash executable (defaults to BASH or bash)')
    args = parser.parse_args()
    bash = shutil.which(args.bash)
    if not bash:
        parser.error(f'Bash executable not found: {args.bash}')
    root = Path(__file__).resolve().parents[2]
    # Use the actual bounded retry implementation, replacing only its sleeps.
    # Its child shell intentionally runs without errexit so it can retry failures.
    retry = (root / 'ci/retry/retry').read_text(encoding='utf-8')
    harness = HARNESS + '\nrun_retry() (\nset +e +u\n' + retry + '\n)\n'
    env = {key: os.environ[key] for key in ('SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP') if key in os.environ}
    env.update(PATH='/usr/bin:/bin', LC_ALL='C')
    count = 0
    with tempfile.TemporaryDirectory(prefix='connectcoin APT update ') as temporary:
        for name, section in apt_sections(root).items():
            for failures in (0, 2, 99):
                fixture = Path(temporary) / f'{name}-{failures}'
                fixture.mkdir()
                result = subprocess.run(
                    [bash, '--noprofile', '--norc', '-c', harness + section, 'apt-update-test', str(failures)],
                    cwd=fixture, env=env, capture_output=True, text=True, encoding='utf-8', timeout=10,
                )
                event_path = fixture / 'events.log'
                events = event_path.read_text(encoding='utf-8').splitlines() if event_path.exists() else []
                exhausted = failures == 99
                expected = [f'update {attempt} 1' for attempt in range(11 if exhausted else failures + 1)]
                if not exhausted:
                    expected.append('install')
                assert result.returncode == (100 if exhausted else 0) and events == expected, (
                    f'{name}, failures={failures}: exit={result.returncode}, events={events}\n'
                    f'stdout={result.stdout}\nstderr={result.stderr}'
                )
                count += 1
    print(f'CI APT updates: {count} isolated success, retry, and exhaustion cases passed')


if __name__ == '__main__':
    main()
