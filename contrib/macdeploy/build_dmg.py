#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Build an English drag-to-Applications DMG from a native static-depends build."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from validate_dmg import APP_NAME, EXECUTABLE, MINIMUM_OS, ValidationError, require, run, validate_app, validate_icon


def make_plist(original, version, arch, minimum):
    require(re.fullmatch(r"\d+\.\d+\.\d+", version), "Version must have three numeric components")
    require(arch in {"arm64", "x86_64"}, "Unsupported architecture")
    require(minimum == MINIMUM_OS, f"This installer targets macOS {MINIMUM_OS} or later")
    require(original.get("CFBundleShortVersionString") == version and original.get("CFBundleVersion") == version,
            "Requested version differs from the CMake-generated application version")
    info = dict(original)
    info.update({"CFBundleIdentifier": "com.connectcoincrypto.ConnectCoinCore", "CFBundleName": "ConnectCoin Core",
                 "CFBundleDisplayName": "ConnectCoin Core", "CFBundleExecutable": "ConnectCoin-Qt",
                 "CFBundlePackageType": "APPL", "CFBundleIconFile": "connectcoin.icns",
                 "CFBundleInfoDictionaryVersion": "6.0", "LSMinimumSystemVersion": minimum,
                 "LSArchitecturePriority": [arch], "NSHighResolutionCapable": True,
                 "CFBundleDevelopmentRegion": "en", "CFBundleSupportedPlatforms": ["MacOSX"],
                 "CFBundleURLTypes": [{"CFBundleURLName": "com.connectcoincrypto.ConnectCoinCore.Payment",
                                        "CFBundleURLSchemes": ["connectcoin"], "CFBundleTypeRole": "Viewer"}]})
    info.pop("LSMinimumSystemVersionByArchitecture", None)
    return info


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_regular_tree(source, destination):
    require(source.is_dir() and not source.is_symlink(), f"Missing regular directory: {source}")
    entries = sorted(source.rglob("*"))
    require(any(entry.is_file() for entry in entries), f"Empty resource directory: {source}")
    for entry in entries:
        require(not entry.is_symlink(), f"Symlinks are not permitted in collected resources: {entry}")
        require(entry.is_dir() or entry.is_file(), f"Non-regular resource: {entry}")
    shutil.copytree(source, destination)


def installation_text(version, arch, source_name):
    label = "Apple Silicon (M-series)" if arch == "arm64" else "Intel (64-bit)"
    return f"""ConnectCoin Core {version} for macOS {MINIMUM_OS} or later - {label}

INSTALL
1. Quit any running ConnectCoin Core application.
2. Drag ConnectCoin Core.app onto the Applications shortcut in this disk image.
3. Eject the disk image and open ConnectCoin Core from Applications.
   Installation does not require editing or deleting your existing wallet data.

SIGNING STATUS
This package is ad-hoc signed, not Developer ID signed, and not notarized.
macOS Gatekeeper may block it. Install only after checking its source and SHA256.
Do not disable Gatekeeper or other system-wide security protections.
Ad-hoc signing is an integrity check, not an Apple developer identity endorsement.

NETWORK AND DATA
Mainnet is the default. Existing explicit network settings are respected.
Back up your wallets before upgrades. Removing the app does not remove wallet data.
This package contains the complete graphical full node and wallet; command-line
utilities are not installed separately into your shell PATH.

SOURCE AND LICENSES
The companion {source_name} contains corresponding source, dependency sources,
patches, and rebuild/relink instructions. Keep it with this installer when sharing.
License and attribution notices are also in the application's Resources/Licenses.
This distribution includes third-party software under its respective licenses.
"""


def build(args):
    require(sys.platform == "darwin", "Building a DMG requires macOS; use --self-test elsewhere")
    repo = Path(__file__).resolve().parents[2]
    build_dir = args.build_dir.resolve(strict=True)
    output = args.output_dir.resolve()
    require(not args.output_dir.is_symlink(), "Output directory must not be a symlink")
    output.mkdir(parents=True, exist_ok=True)
    name = f"connectcoin-core-{args.version}-macos-{args.arch}"
    targets = [output / f"{name}.dmg", output / f"build-{args.arch}.json", output / f"SHA256SUMS-{args.arch}.txt"]
    require(not any(path.exists() or path.is_symlink() for path in targets), "Refusing to overwrite an existing installer/report")
    source = output / f"connectcoin-{args.version}-macos-{args.arch}-sources.tar.gz"
    require(source.is_file() and not source.is_symlink(), "Create the companion sources with collect_sources.py first")
    commit = run(["git", "-C", repo, "rev-parse", "HEAD"]).stdout.strip()
    require(not run(["git", "-C", repo, "status", "--porcelain", "--untracked-files=normal"]).stdout.strip(),
            "Package only committed source; keep build/output directories ignored")
    binary = build_dir / "bin/connectcoin-qt"
    require(binary.is_file() and not binary.is_symlink(), "Build connectcoin-qt first")
    with (build_dir / "ConnectCoin-Qt.app/Contents/Info.plist").open("rb") as stream:
        info = make_plist(plistlib.load(stream), args.version, args.arch, args.minimum_macos)
    icon = repo / "src/qt/res/icons/connectcoin.icns"
    validate_icon(icon.read_bytes())
    # Qt's qt_*.qm catalogs can depend on qtbase_*.qm and other catalogs.
    # Keep the complete compiled translation set rather than only the wrappers.
    translations = sorted(args.qt_translations.glob("*.qm"))
    require(translations and all(path.is_file() and not path.is_symlink() for path in translations),
            "Missing compiled Qt translations from the depends prefix")

    # Every staging path belongs to this fresh temporary directory. Existing
    # app installations, mounted images, wallet files and old output stay intact.
    with tempfile.TemporaryDirectory(prefix="connectcoin-dmg-build-", dir=output) as temporary:
        temporary = Path(temporary)
        volume = temporary / "volume"
        app = volume / APP_NAME
        resources = app / "Contents/Resources"
        resources.mkdir(parents=True)
        executable = app / EXECUTABLE
        executable.parent.mkdir()
        shutil.copy2(binary, executable)
        executable.chmod(0o755)
        run(["/usr/bin/xcrun", "strip", "-S", "-x", executable])
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info, sort_keys=True))
        (app / "Contents/PkgInfo").write_bytes(b"APPL????")
        shutil.copy2(icon, resources / "connectcoin.icns")
        (resources / "Base.lproj").mkdir()
        (resources / "Base.lproj/InfoPlist.strings").write_text(
            '"CFBundleDisplayName" = "ConnectCoin Core";\n"CFBundleName" = "ConnectCoin Core";\n', encoding="utf-8")
        for translation in translations:
            shutil.copy2(translation, resources / translation.name)
        (resources / "qt.conf").write_text("[Paths]\nTranslations=Resources\n", encoding="utf-8")
        copy_regular_tree(args.licenses_dir, resources / "Licenses")
        install_text = installation_text(args.version, args.arch, source.name)
        (volume / "INSTALL.txt").write_text(install_text, encoding="utf-8")
        (volume / "Applications").symlink_to("/Applications", target_is_directory=True)
        run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", app], timeout=120)
        images = validate_app(app.resolve(), args.arch, args.version)
        dmg = temporary / targets[0].name
        run(["/usr/bin/hdiutil", "create", "-volname", f"ConnectCoin Core {args.version}", "-srcfolder", volume,
             "-format", "UDZO", "-fs", "HFS+", dmg], timeout=180)
        run(["/usr/bin/hdiutil", "verify", dmg], timeout=120)
        provenance = {"version": args.version, "architecture": args.arch, "minimum_macos": args.minimum_macos,
                      "source_commit": commit, "qt": "6.8.4 (static, repository depends)",
                      "signature": "ad-hoc", "developer_id_signed": False, "notarized": False,
                      "xcode": run(["/usr/bin/xcodebuild", "-version"]).stdout.strip(),
                      "sdk": run(["/usr/bin/xcrun", "--show-sdk-version"]).stdout.strip(),
                      "mach_o_images": images, "dmg_sha256": sha256(dmg),
                      "corresponding_sources": source.name, "sources_sha256": sha256(source)}
        report = temporary / targets[1].name
        report.write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
        checksums = temporary / targets[2].name
        checksums.write_text("".join(f"{sha256(path)}  {path.name}\n" for path in (dmg, source, report)), encoding="utf-8")
        # link() atomically refuses a racing existing destination, unlike replace().
        for staged, final in zip((dmg, report, checksums), targets):
            final.hardlink_to(staged)
    print(json.dumps({"dmg": str(targets[0]), "source_commit": commit, "signature": "ad-hoc", "notarized": False}))


class RegressionTests(unittest.TestCase):
    def test_metadata_and_version(self):
        from validate_dmg import validate_plist
        original = {"CFBundleShortVersionString": "1.0.0", "CFBundleVersion": "1.0.0",
                    "LSArchitecturePriority": ["x86_64"], "CFBundleIdentifier": "invalid.old"}
        for arch in ("arm64", "x86_64"):
            info = make_plist(original, "1.0.0", arch, "15.0")
            validate_plist(info, "1.0.0")
            self.assertEqual(info["LSArchitecturePriority"], [arch])
        self.assertEqual(original["CFBundleIdentifier"], "invalid.old")
        for version, arch, minimum in (("1.0.1", "arm64", "15.0"), ("../1", "arm64", "15.0"),
                                       ("1.0.0", "other", "15.0"), ("1.0.0", "arm64", "14.0")):
            with self.subTest(version=version, arch=arch, minimum=minimum), self.assertRaises(ValidationError):
                make_plist(original, version, arch, minimum)

    def test_english_installation_discloses_signing_and_sources(self):
        text = installation_text("1.0.0", "arm64", "sources.tar.gz")
        for expected in ("Applications", "not notarized", "Apple Silicon", "sources.tar.gz", "Mainnet"):
            self.assertIn(expected, text)

    def test_license_copy_and_empty_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "notices"
            source.mkdir()
            with self.assertRaises(ValidationError):
                copy_regular_tree(source, root / "output")
            (source / "COPYING").write_text("fixture", encoding="utf-8")
            copy_regular_tree(source, root / "output")
            self.assertEqual((root / "output/COPYING").read_text(encoding="utf-8"), "fixture")

    def test_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fixture"
            path.write_bytes(b"abc")
            self.assertEqual(sha256(path), hashlib.sha256(b"abc").hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--arch", choices=("arm64", "x86_64"))
    parser.add_argument("--minimum-macos", default=MINIMUM_OS)
    parser.add_argument("--qt-translations", type=Path)
    parser.add_argument("--licenses-dir", type=Path)
    args = parser.parse_args()
    if args.self_test:
        unittest.main(argv=[sys.argv[0]], exit=True)
    for name in ("build_dir", "output_dir", "version", "arch", "qt_translations", "licenses_dir"):
        if getattr(args, name) is None:
            parser.error(f"--{name.replace('_', '-')} is required")
    try:
        build(args)
    except (ValidationError, OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
