#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that POSIX startup raises the soft file descriptor limit without changing the parent."""

import os
import subprocess
import sys

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal


class FileDescriptorLimitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.uses_wallet = False

    def skip_test_if_missing_module(self):
        if os.name != "posix":
            raise SkipTest("POSIX file descriptor limits are required")
        if self.options.valgrind:
            raise SkipTest("Valgrind virtualizes file descriptor limits")
        try:
            import resource
        except ImportError:
            raise SkipTest("Python resource module is required")
        if not hasattr(resource, "RLIMIT_NOFILE"):
            raise SkipTest("RLIMIT_NOFILE is unavailable")

        self.parent_limits = resource.getrlimit(resource.RLIMIT_NOFILE)
        parent_hard = self.parent_limits[1]
        requested_hard = 4096 if parent_hard == resource.RLIM_INFINITY else min(4096, parent_hard)
        # macOS/BSD can reject an inherited hard limit or clamp a requested one
        # to a lower kernel ceiling. Find an accepted, finite test budget in a
        # disposable process; never lower the framework's own hard limit.
        probe_limit = """
import resource
import sys

candidate = int(sys.argv[1])
while candidate >= 1024:
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (candidate, candidate))
    except (OSError, ValueError):
        candidate //= 2
        continue
    observed = resource.getrlimit(resource.RLIMIT_NOFILE)
    print(min([candidate, *(value for value in observed if value != resource.RLIM_INFINITY)]))
    break
else:
    print(0)
"""
        self.child_hard = int(subprocess.check_output(
            [sys.executable, "-c", probe_limit, str(requested_hard)], text=True, timeout=10,
        ))
        assert_equal(resource.getrlimit(resource.RLIMIT_NOFILE), self.parent_limits)
        if self.child_hard < 1024:
            raise SkipTest("An accepted hard file descriptor limit of at least 1024 is required")

    def setup_network(self):
        self.add_nodes(self.num_nodes)
        # assert_debug_log() needs an existing file before the first startup.
        self.nodes[0].debug_log_path.parent.mkdir(exist_ok=True)
        self.nodes[0].debug_log_path.touch()

    def run_test(self):
        import resource

        node = self.nodes[0]
        node_args = node.args
        # The framework has background threads, so use a fresh interpreter rather
        # than preexec_fn. Only this child lowers its hard limit, then exec keeps
        # the same PID so Linux prlimit can also check the running node directly.
        set_limits_and_exec = """
import os
import resource
import sys

resource.setrlimit(resource.RLIMIT_NOFILE, (int(sys.argv[1]), int(sys.argv[2])))
os.execvp(sys.argv[3], sys.argv[3:])
"""

        # Cover a soft limit below the startup requirement, one already above
        # that requirement, and one already at the hard limit. In every case the
        # hard limit is well above the requirement for -maxconnections=16.
        for child_soft in (128, self.child_hard // 2, self.child_hard):
            self.log.info(f"Starting with fd limits soft={child_soft}, hard={self.child_hard}")
            node.args = [
                sys.executable, "-c", set_limits_and_exec,
                str(child_soft), str(self.child_hard), *node_args,
            ]
            try:
                with node.assert_debug_log([
                    f"File descriptor limits: soft={self.child_hard}, hard={self.child_hard}",
                ]):
                    self.start_node(0, extra_args=["-maxconnections=16", "-privatebroadcast=0"])
                if sys.platform.startswith("linux") and hasattr(resource, "prlimit"):
                    assert_equal(resource.prlimit(node.process.pid, resource.RLIMIT_NOFILE),
                                 (self.child_hard, self.child_hard))
                assert_equal(resource.getrlimit(resource.RLIMIT_NOFILE), self.parent_limits)
            finally:
                if node.running:
                    self.stop_node(0)
                node.args = node_args


if __name__ == '__main__':
    FileDescriptorLimitTest(__file__).main()
