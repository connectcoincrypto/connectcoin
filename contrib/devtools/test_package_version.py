# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Regressions for the single-source installer version and workflow wiring."""

from pathlib import Path
import re
import unittest

from package_version import package_version


SOURCE = """set(CLIENT_VERSION_MAJOR 1)
set(CLIENT_VERSION_MINOR 0)
set(CLIENT_VERSION_BUILD 1)
set(CLIENT_VERSION_RC 0)
"""


class PackageVersionTests(unittest.TestCase):
    def test_release_version(self):
        self.assertEqual(package_version(SOURCE), "1.0.1")
        self.assertEqual(package_version(SOURCE.replace("BUILD 1", "BUILD 42")), "1.0.42")

    def test_comments_and_whitespace(self):
        source = "# set(CLIENT_VERSION_BUILD 99)\n" + SOURCE.replace(
            "set(CLIENT_VERSION_BUILD 1)", "  SET ( CLIENT_VERSION_BUILD 1 ) # patch release")
        self.assertEqual(package_version(source.replace("\n", "\r\n")), "1.0.1")

    def test_missing_and_duplicate_definitions(self):
        for part in ("MAJOR", "MINOR", "BUILD", "RC"):
            line = next(line for line in SOURCE.splitlines(keepends=True) if f"CLIENT_VERSION_{part} " in line)
            with self.subTest(part=part, case="missing"), self.assertRaisesRegex(ValueError, "Missing"):
                package_version(SOURCE.replace(line, ""))
            with self.subTest(part=part, case="duplicate"), self.assertRaisesRegex(ValueError, "Duplicate"):
                package_version(SOURCE + line)
            with self.subTest(part=part, case="multiline duplicate"), self.assertRaisesRegex(ValueError, "Duplicate"):
                package_version(SOURCE + line.replace("set(", "set(\n"))
            with self.subTest(part=part, case="inline duplicate"), self.assertRaisesRegex(ValueError, "Duplicate"):
                package_version(SOURCE + "set(UNRELATED 0) " + line)

    def test_malformed_definitions(self):
        for value in ('"1"', "-1", "+1", "01", "1.0", "${PATCH}", "1 CACHE STRING patch", "1) set(OTHER 2", "1\n", ""):
            with self.subTest(value=value), self.assertRaises(ValueError):
                package_version(SOURCE.replace("BUILD 1", f"BUILD {value}"))
        with self.assertRaises(ValueError):
            package_version(SOURCE.replace("BUILD 1)", "BUILD 1"))

    def test_prerelease_is_not_silently_packaged_as_stable(self):
        with self.assertRaisesRegex(ValueError, "CLIENT_VERSION_RC=0"):
            package_version(SOURCE.replace("RC 0", "RC 1"))

    def test_installer_workflows_share_version_and_commit(self):
        root = Path(__file__).resolve().parents[2]
        for name in ("linux-packages.yml", "macos-packages.yml"):
            source = (root / ".github/workflows" / name).read_text(encoding="utf-8")
            with self.subTest(workflow=name):
                self.assertIn("python3 contrib/devtools/package_version.py", source)
                self.assertIn("python3 -m unittest discover -s contrib/devtools -p 'test_package_version.py' -v", source)
                self.assertIn("PACKAGE_VERSION: ${{ needs.metadata.outputs.version }}", source)
                self.assertNotRegex(source, r"connectcoin-core-\d+\.\d+\.\d+")
                self.assertNotRegex(source, r"--version\s+\d+\.\d+\.\d+")
                for checkout in re.finditer(r"- uses: actions/checkout@[^\n]+\n((?:[ ]{8,}[^\n]*\n)+)", source):
                    self.assertIn("ref: ${{ github.sha }}", checkout[1])
        linux = (root / ".github/workflows/linux-packages.yml").read_text(encoding="utf-8")
        self.assertIn("needs: [metadata, build]", linux)
        self.assertIn("name: connectcoin-core-${{ needs.metadata.outputs.version }}-${{ matrix.artifact }}-x86_64", linux)


if __name__ == "__main__":
    unittest.main()
