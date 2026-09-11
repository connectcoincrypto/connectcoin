# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Isolated execution of actual CI marker blocks and actual YAML predicates.

Never sources/runs a whole CI script. All fixture effects stay in temporary directories.
No dependency builds, containers, cache uploads, or network access occur.
"""
import ast
import itertools
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which('bash')
HOST_SOURCE = (ROOT / 'ci/test_run_all.sh').read_text(encoding='utf-8')
GUEST_SOURCE = (ROOT / 'ci/test/03_test_script.sh').read_text(encoding='utf-8')
SAVE_SOURCE = (ROOT / '.github/actions/cache/save/action.yml').read_text(encoding='utf-8')
INTERNAL_SOURCE = (ROOT / '.github/actions/cache/save/internal/action.yml').read_text(encoding='utf-8')
WORKFLOWS = '\n'.join((ROOT / '.github/workflows' / name).read_text(encoding='utf-8')
                      for name in ('ci.yml', 'ci-windows-cross.yml'))
HOST_PREFIX = HOST_SOURCE.split('source ./ci/test/00_setup_env.sh', 1)[0]
GUEST_PREFIX = GUEST_SOURCE.split('cd "${BASE_ROOT_DIR}"', 1)[0]
DEPENDS_BLOCK = GUEST_SOURCE[GUEST_SOURCE.index('if [ -z "$NO_DEPENDS" ]; then'):].split('CONNECTCOIN_CONFIG_ALL=', 1)[0]
assert '.ci-depends-complete' in HOST_PREFIX and '.ci-depends-complete' in GUEST_PREFIX
assert 'make $MAKEJOBS' in DEPENDS_BLOCK and 'CI_DEPENDS_CACHE_RUN' in DEPENDS_BLOCK


def step_blocks(source):
    """Extract this repository's step blocks; not a general YAML parser."""
    matches = list(re.finditer(r'^(\s*)- name: (.+)$', source, re.M))
    return [(m.group(2), source[m.end():matches[i+1].start() if i+1 < len(matches) else len(source)])
            for i, m in enumerate(matches)]


def predicate(block):
    return re.search(r'^\s+if: \$\{\{ (.+) \}\}\s*$', block, re.M).group(1)


SAVE_STEPS = dict(step_blocks(SAVE_SOURCE))
VALIDATION = textwrap.dedent(SAVE_STEPS['Check completed dependency build'].split('run: |\n', 1)[1])


def evaluate(expression, context, status):
    # Lex strings first so their contents are not rewritten as identifiers.
    matches = list(re.finditer(r"'[^']*'|\b(?:github|env|inputs|steps)\.[A-Za-z0-9_.-]+|&&|\|\||!=|==|[()]|[A-Za-z_]+", expression))
    end = 0
    for match in matches:
        assert not expression[end:match.start()].strip(), expression[end:match.start()]
        end = match.end()
    assert not expression[end:].strip(), expression[end:]
    pieces = [match.group() for match in matches]
    transformed = []
    for token in pieces:
        if re.match(r'^(github|env|inputs|steps)\.', token):
            assert token in context, token
            transformed.append(repr(context[token]))
        else:
            transformed.append({'&&': 'and', '||': 'or'}.get(token, token))
    tree = ast.parse(' '.join(transformed), mode='eval')

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, ast.BoolOp):
            values = [bool(walk(v)) for v in node.values]
            return all(values) if isinstance(node.op, ast.And) else any(values)
        if isinstance(node, ast.Compare):
            assert len(node.ops) == 1 and isinstance(node.ops[0], (ast.Eq, ast.NotEq))
            equal = walk(node.left) == walk(node.comparators[0])
            return equal if isinstance(node.ops[0], ast.Eq) else not equal
        if isinstance(node, ast.Call):
            assert isinstance(node.func, ast.Name) and node.func.id in ('success', 'failure')
            assert not node.args and not node.keywords
            return node.func.id == status
        raise AssertionError(ast.dump(node))

    return bool(walk(tree))


class DependsCacheTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='connectcoin cache case ')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        self.build = self.directory / 'build with spaces ₿🧪'
        self.build.mkdir()
        self.marker = self.build / '.ci-depends-complete'
        self.output = self.directory / 'github output'
        self.make_log = self.directory / 'make calls'
        self.token = '34400817435:2:ci-matrix:ci_native_fuzz_0'
        self.env = os.environ | {
            'BASE_BUILD_DIR': self.build.as_posix(),
            'CI_DEPENDS_CACHE_RUN': self.token,
            'CI_CACHE_RUN': self.token,
            'GITHUB_OUTPUT': self.output.as_posix(),
            'DANGER_RUN_CI_ON_HOST': '1',
            'NO_DEPENDS': '', 'CI_IMAGE_NAME_TAG': 'ubuntu',
            'MAKEJOBS': '-j2', 'HOST': 'fake-host', 'DEP_OPTS': '',
            'MOCK_MAKE_EXIT': '0', 'MOCK_MAKE_LOG': self.make_log.as_posix(),
            'GITHUB_RUN_ID': '34400817435', 'GITHUB_RUN_ATTEMPT': '2',
            'GITHUB_JOB': 'ci-matrix', 'CONTAINER_NAME': 'ci_native_fuzz_0',
        }
        self.mock_make = '''
make() { printf 'called\\n' >> "$MOCK_MAKE_LOG"; return "$MOCK_MAKE_EXIT"; }
export -f make
'''

    def run_shell(self, body, expected=0, **changes):
        if BASH is None:
            self.skipTest('bash is not installed')
        result = subprocess.run([BASH, '--noprofile', '--norc', '-c', body],
                                cwd=self.directory, env=self.env | changes,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def complete(self, **changes):
        self.run_shell(VALIDATION, **changes)
        return self.output.read_text(encoding='utf-8').splitlines()[-1] == 'complete=true'

    def test_host_invalidates_before_setup_or_docker_failure(self):
        self.marker.write_text(self.token)
        self.run_shell(HOST_PREFIX + '\nexit 43\n', expected=43)
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.complete())
        result = self.run_shell(HOST_PREFIX + '\nprintf "%s" "$CI_DEPENDS_CACHE_RUN"\n', CI_DEPENDS_CACHE_RUN='older-invocation')
        self.assertEqual(result.stdout, self.token)

    def test_guest_invalidates_before_danger_guard_or_early_failure(self):
        for danger, expected in [('0', 1), ('1', 44)]:
            with self.subTest(danger=danger):
                self.marker.write_text(self.token)
                self.run_shell(GUEST_PREFIX + '\nexit 44\n', expected=expected, DANGER_RUN_CI_ON_HOST=danger)
                self.assertFalse(self.marker.exists())
                self.assertFalse(self.complete())

    def test_depends_failure_never_publishes_even_with_stale_marker(self):
        self.marker.write_text(self.token)
        self.run_shell(GUEST_PREFIX + self.mock_make + DEPENDS_BLOCK, expected=45, MOCK_MAKE_EXIT='45')
        self.assertEqual(self.make_log.read_text(encoding='utf-8').splitlines(), ['called'])
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.complete())

    def test_depends_success_survives_later_failure(self):
        for later_exit in (0, 47):
            with self.subTest(later_exit=later_exit):
                self.marker.write_text('older-run')
                self.run_shell(GUEST_PREFIX + self.mock_make + DEPENDS_BLOCK + f'\nexit {later_exit}\n', expected=later_exit)
                self.assertEqual(self.marker.read_text(encoding='utf-8').strip(), self.token)
                self.assertTrue(self.complete())

    def test_no_depends_and_missing_configuration_fail_closed(self):
        self.marker.write_text(self.token)
        self.run_shell(GUEST_PREFIX + self.mock_make + DEPENDS_BLOCK, NO_DEPENDS='1')
        self.assertFalse(self.make_log.exists())
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.complete())
        for changes in ({'BASE_BUILD_DIR': ''}, {'CI_DEPENDS_CACHE_RUN': ''}):
            with self.subTest(changes=changes):
                self.run_shell(GUEST_PREFIX + self.mock_make + DEPENDS_BLOCK, **changes)
                self.assertFalse(self.marker.exists())
                self.assertFalse(self.complete())

    def test_run_identity_mismatch_rejected(self):
        for value in ('', 'older-run', self.token.replace(':2:', ':1:'), self.token + ':extra'):
            self.marker.write_text(value)
            self.assertFalse(self.complete())
        self.marker.write_text(self.token)
        self.assertTrue(self.complete())
        self.assertFalse(self.complete(BASE_BUILD_DIR=''))
        self.assertFalse(self.complete(CI_CACHE_RUN=''))

    def test_symlink_marker_rejected(self):
        target = self.directory / 'marker target'
        target.write_text(self.token)
        try:
            self.marker.symlink_to(target)
        except OSError as error:
            if os.name == 'nt' and getattr(error, 'winerror', None) == 1314:
                self.skipTest(f'Symlink privilege unavailable: {error}')
            raise
        self.assertTrue(self.marker.is_symlink())
        self.assertFalse(self.complete())

    def test_marker_token_is_forwarded_to_container(self):
        source = (ROOT / 'ci/test/02_run_container.py').read_text(encoding='utf-8')
        self.assertIn('"CI_DEPENDS_CACHE_RUN",', source)
        self.assertIn('CI_CACHE_RUN: ${{ github.run_id }}:${{ github.run_attempt }}:${{ github.job }}:${{ env.CONTAINER_NAME }}', SAVE_SOURCE)

    def test_real_yaml_gates(self):
        checks = 0
        for status, event, provider, branch, shard, complete, sources_hit, built_hit in itertools.product(
                ('success', 'failure', 'cancelled'), ('push', 'pull_request', 'workflow_dispatch'), ('gha', 'warp'),
                ('main', 'feature'), ('', '0', '1'), ('false', 'true'), ('false', 'true'), ('false', 'true')):
            context = {
                'github.event_name': event, 'github.ref_name': branch,
                'github.event.repository.default_branch': 'main', 'inputs.provider': provider,
                'env.FUZZ_SHARD_COUNT': '' if shard == '' else '4', 'env.FUZZ_SHARD_INDEX': shard,
                'steps.depends-status.outputs.complete': complete,
                'env.depends-sources-cache-hit': sources_hit, 'env.depends-built-cache-hit': built_hit,
            }
            allowed = status != 'cancelled' and event == 'push' and (provider == 'gha' or branch == 'main') and shard != '1'
            for name in ('Save Ccache cache', 'Save depends sources cache', 'Save built depends cache'):
                cache_hit = sources_hit if name == 'Save depends sources cache' else built_hit
                expected = allowed and (name == 'Save Ccache cache' or (complete == 'true' and cache_hit != 'true'))
                self.assertEqual(evaluate(predicate(SAVE_STEPS[name]), context, status), expected,
                                 (name, status, event, provider, branch, shard, complete, sources_hit, built_hit))
                checks += 1
        print(f'Validated {checks} cache-export predicate combinations', flush=True)

    def test_workflow_and_internal_gates(self):
        steps = [block for name, block in step_blocks(WORKFLOWS) if name == 'Save caches']
        self.assertEqual(len(steps), 2)
        self.assertNotIn('name: Save Ccache cache after failure', WORKFLOWS)
        for block in steps:
            self.assertIn('uses: ./.github/actions/cache/save', block)
            expression = predicate(block)
            for status, event, shard in itertools.product(('success', 'failure', 'cancelled'), ('push', 'pull_request'), ('', '0', '1')):
                context = {'github.event_name': event, 'env.FUZZ_SHARD_COUNT': '' if shard == '' else '4', 'env.FUZZ_SHARD_INDEX': shard}
                expected = status != 'cancelled' and event == 'push' and (shard != '1' or 'FUZZ_SHARD' not in expression)
                self.assertEqual(evaluate(expression, context, status), expected)
        for name, block in step_blocks(INTERNAL_SOURCE):
            for status, provider in itertools.product(('success', 'failure', 'cancelled'), ('gha', 'warp')):
                expected = status != 'cancelled' and provider == ('warp' if 'WarpBuild' in name else 'gha')
                self.assertEqual(evaluate(predicate(block), {'inputs.provider': provider}, status), expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
