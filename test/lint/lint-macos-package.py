#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Run cross-platform macOS packaging, validation and source-bundle regressions."""

from pathlib import Path
import re
import subprocess
import sys
import unittest


class WorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/macos-packages.yml").read_text(encoding="utf-8")

    def step(self, name):
        # Check this small workflow's source contracts without adding a YAML
        # dependency to the cross-platform packaging lint.
        match = re.search(r"^      - name: " + re.escape(name) + r"\n(.*?)(?=^      - |\Z)",
                          self.workflow, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, f"Missing macOS workflow step: {name}")
        return match[1]

    def test_dedicated_branch_pushes_have_no_path_filter(self):
        push = re.search(r"^  push:\n((?:^    .*\n|^\s*\n)*)", self.workflow, re.MULTILINE)
        self.assertIsNotNone(push, "The dedicated packaging branch needs push coverage")
        self.assertIn("branches: ['codex/macos-installers']", push[1])
        self.assertNotRegex(push[1], r"(?m)^    paths(?:-ignore)?:")

    def test_dependency_cache_uses_the_normalized_host(self):
        setup = self.step("Verify native toolchain and install dependencies")
        self.assertIn('DEPENDS_HOST=$(cd depends && ./config.sub "$(./config.guess)")', setup)
        restore = self.step("Restore pinned dependency packages")
        key = re.search(r"^          key: (.+)$", restore, re.MULTILINE)
        self.assertIsNotNone(key, "Missing dependency cache key")
        for component in ("${{ matrix.arch }}", "${{ env.DEPENDS_HOST }}", "${{ hashFiles('depends/**') }}"):
            self.assertIn(component, key[1])
        self.assertLess(self.workflow.index(setup), self.workflow.index(restore))
        self.assertIn("key: ${{ steps.depends-cache.outputs.cache-primary-key }}",
                      self.step("Save completed dependency packages"))

    def test_precompressed_artifacts_are_not_recompressed(self):
        upload = self.step("Upload installers and verification reports")
        self.assertRegex(upload, r"(?m)^          compression-level: 0(?:\s*(?:#.*)?)?$")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WorkflowTests)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        sys.exit(1)
    helpers = Path(__file__).resolve().parents[2] / "contrib/macdeploy"
    for name in ("build_dmg.py", "validate_dmg.py", "collect_sources.py"):
        subprocess.run([sys.executable, str(helpers / name), "--self-test"], check=True)
