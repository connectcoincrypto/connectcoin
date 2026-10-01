#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Collect corresponding source and notices for native, static-depends macOS builds.

No archive is extracted and no dependency code is executed. Source inputs are
checked against the pinned depends recipes before their notices are copied.
"""

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


MINIMUM_MACOS = "15.0"
MAX_NOTICE_SIZE = 8 * 1024 * 1024
REPO_NOTICES = (
    "COPYING", "src/randomx/LICENSE", "src/crc32c/LICENSE", "src/leveldb/LICENSE",
    "src/minisketch/LICENSE", "src/secp256k1/COPYING", "src/crypto/ctaes/COPYING",
)


def run_git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contained(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def safe_name(name):
    """Accept portable relative archive names, without normalizing traversal away."""
    if not name or "\\" in name or ":" in name or any(ord(char) < 32 for char in name):
        raise ValueError(f"Unsafe archive name: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"Unsafe archive name: {name!r}")
    return path


def check_member(member):
    path = safe_name(member.name)
    if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
        raise ValueError(f"Unsupported archive entry: {member.name}")
    if member.issym() or member.islnk():
        target = member.linkname
        if not target or "\\" in target or ":" in target or target.startswith("/"):
            raise ValueError(f"Unsafe archive link: {member.name}")
        base = str(path.parent) if member.issym() else ""
        resolved = posixpath.normpath(posixpath.join(base, target))
        safe_name(resolved)
        # A dependency's top-level source directory is the link's trust boundary.
        if PurePosixPath(resolved).parts[0] != path.parts[0]:
            raise ValueError(f"Archive link escapes source root: {member.name}")


def is_notice(name):
    path = PurePosixPath(name)
    base = path.name.lower()
    return (bool(re.match(r"^(?:copying|copyright|licen[cs]e|notice|authors)(?:[._-]|$)", base))
            or any(part.lower() in {"licenses", "licences"} for part in path.parts)
            or base == "qt_attribution.json" or base == "qt_attributions.json"
            or base.startswith("readme") and (len(path.parts) <= 2 or "3rdparty" in path.parts))


def assignment(text, key):
    match = re.search(r"^" + re.escape(key) + r"\s*(?::=|=)\s*([^\r\n#]+)", text, re.MULTILINE)
    if not match:
        raise ValueError(f"Missing pinned depends value: {key}")
    return match.group(1).strip()


def pinned_sources(repo):
    """Read only the simple, explicitly supported recipe fields, never invoke make."""
    result = {}
    for package in ("boost", "qrencode", "sqlite", "zeromq"):
        recipe = (repo / "depends/packages" / (package + ".mk")).read_text(encoding="utf-8")
        version = assignment(recipe, "$(package)_version")
        name = assignment(recipe, "$(package)_file_name")
        name = name.replace("$($(package)_version)", version).replace("$(package)", package)
        result[name] = assignment(recipe, "$(package)_sha256_hash")
    recipe = (repo / "depends/packages/qt_details.mk").read_text(encoding="utf-8")
    version = assignment(recipe, "qt_details_version")
    suffix = assignment(recipe, "qt_details_suffix").replace("$(qt_details_version)", version)
    for component in ("qtbase", "qttools", "qttranslations"):
        name = assignment(recipe, f"qt_details_{component}_file_name").replace("$(qt_details_suffix)", suffix)
        result[name] = assignment(recipe, f"qt_details_{component}_sha256_hash")
    for component in ("top_cmakelists", "top_cmake_ecmoptionaladdsubdirectory", "top_cmake_qttoplevelhelpers"):
        name = assignment(recipe, f"qt_details_{component}_file_name") + "-" + version
        result[name] = assignment(recipe, f"qt_details_{component}_sha256_hash")
    for name, digest in result.items():
        if safe_name(name).name != name or "$" in name or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"Unsupported depends source declaration: {name}")
    return result


def copy_notice(destination, origin, data, inventory):
    if not data or len(data) > MAX_NOTICE_SIZE:
        raise ValueError(f"Missing or oversized notice: {origin}")
    # Flatten names with an origin hash: never let an archive path control writes.
    filename = re.sub(r"[^A-Za-z0-9._-]", "_", PurePosixPath(origin).name)[:100]
    filename = hashlib.sha256(origin.encode()).hexdigest()[:16] + "-" + filename
    (destination / filename).write_bytes(data)
    inventory.append({"origin": origin, "file": filename, "sha256": hashlib.sha256(data).hexdigest()})


def archive_notices(path, destination, inventory):
    count = 0
    seen = set()
    with tarfile.open(path, "r:*") as source:
        for member in source:
            check_member(member)
            if member.name in seen:
                raise ValueError(f"Duplicate archive member: {member.name}")
            seen.add(member.name)
            if member.isfile() and is_notice(member.name):
                if member.size > MAX_NOTICE_SIZE:
                    raise ValueError(f"Oversized notice: {member.name}")
                with source.extractfile(member) as notice:
                    data = notice.read(MAX_NOTICE_SIZE + 1)
                if data:  # Ignore upstream empty placeholder README files.
                    copy_notice(destination, path.name + "/" + member.name, data, inventory)
                    count += 1
    if not count:
        raise ValueError(f"No license/notice material found in {path.name}")


def mbedtls_source(build_dir):
    cache = build_dir / "CMakeCache.txt"
    source = build_dir / "_deps/connectcoin_mbedtls-src"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("FETCHCONTENT_SOURCE_DIR_CONNECTCOIN_MBEDTLS:PATH="):
                override = line.partition("=")[2]
                if override:
                    source = Path(override)
                    if not source.is_absolute():
                        raise ValueError("Mbed TLS source override must be absolute")
    source = source.resolve(strict=True)
    for name in ("LICENSE", "CMakeLists.txt", "include/mbedtls/version.h", "library/rsa.c"):
        if not (source / name).is_file():
            raise ValueError(f"Incomplete Mbed TLS source: missing {name}")
    return source


def source_tree_files(root):
    for current, directories, files in os.walk(root, followlinks=False):
        directories[:] = sorted(name for name in directories if name != ".git")
        base = Path(current)
        for name in directories + sorted(files):
            if name == ".git":
                continue
            path = base / name
            relative = path.relative_to(root).as_posix()
            safe_name(relative)
            if path.is_symlink():
                if not contained(path.resolve(strict=True), root):
                    raise ValueError(f"Mbed TLS symlink escapes source: {relative}")
                raise ValueError(f"Mbed TLS source symlinks are not supported: {relative}")
            if not path.is_dir():
                if not path.is_file():
                    raise ValueError(f"Unsupported source entry: {relative}")
                yield path, relative


def add_bytes(archive, name, data, epoch, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size, info.mtime, info.mode = len(data), epoch, mode
    archive.addfile(info, io.BytesIO(data))


def add_file(archive, name, path, epoch):
    info = tarfile.TarInfo(name)
    info.size, info.mtime = path.stat().st_size, epoch
    info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
    with path.open("rb") as source:
        archive.addfile(info, source)


def rebuild_text(commit, version, arch):
    return f"""ConnectCoin Core {version}: corresponding source for macOS {arch}
Source commit: {commit}
Minimum supported macOS: {MINIMUM_MACOS} (deployment target, NOT the SDK version)

This archive contains the complete tracked ConnectCoin tree and its patches,
the exact SHA-256-verified depends source inputs, and the populated Mbed TLS
source including ConnectCoin modifications. It does not contain Apple's SDK,
private signing keys, compiler binaries, or a claim of bit-for-bit reproduction.

On a native {arch} Mac running macOS 15 or later, install Xcode 16.2 and obtain
Apple's SDK from Apple. Select that Xcode with xcode-select. The
SDK version is determined by the selected Xcode, not by the 15.0 deployment
target. See build-environment.json for the original SDK/compiler information.
Install build tools (Homebrew): cmake make ninja pkgconf python coreutils.
From this archive's source/connectcoin directory:

  mkdir -p depends/sources
  cp ../depends-sources/* depends/sources/
  native_host=$(./depends/config.sub "$(./depends/config.guess)")
  gmake -C depends -j2 NO_IPC=1 NO_USDT=1 XCODE_VERSION=16.2 \\
    OSX_MIN_VERSION=15.0 OSX_SDK="$(xcrun --show-sdk-path)" \\
    OSX_SDK_VERSION="$(xcrun --show-sdk-version)"
  cmake -S . -B build-macos -G Ninja \\
    --toolchain "depends/$native_host/toolchain.cmake" \\
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \\
    -DCMAKE_OSX_ARCHITECTURES={arch} -DBUILD_GUI=ON -DENABLE_IPC=OFF -DWITH_USDT=OFF \\
    -DFETCHCONTENT_SOURCE_DIR_CONNECTCOIN_MBEDTLS="$(cd ../mbedtls && pwd)"
  cmake --build build-macos -j2 --target connectcoin connectcoin-qt \\
    connectcoind connectcoin-cli connectcoin-tx connectcoin-wallet connectcoin-util

You may modify Qt/QRencode and rebuild/relink the application. For a modified
depends input, update its recipe SHA-256 and use fresh depends/build outputs.
The source archive uses git archive and does not contain .git; the original
commit is recorded above. Modified builds must not claim the original hash.
To use the Git-checking package tools after rebuilding, initialize a new local
Git repository in source/connectcoin (git init; git add .; git commit), recording
your modifications as a new commit. Recollect sources into a fresh output
directory and supply that new licenses directory to build_dmg.py. The newly
created local commit identifies YOUR rebuild, not the original release. See the
included contrib/macdeploy/README.md for the package tool arguments. Ad-hoc-sign
the resulting application after all changes.
No project-owned Developer ID or notarization credential is supplied or needed
for a local rebuild. See contrib/macdeploy/README.md for package usage and
signature limitations. No system service or wallet-data installation is needed.
"""


def build_environment(build_dir):
    result = {"minimum_macos": MINIMUM_MACOS}
    wanted = {"CMAKE_BUILD_TYPE", "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_OSX_ARCHITECTURES",
              "CMAKE_OSX_SYSROOT", "CMAKE_CXX_COMPILER", "CMAKE_C_COMPILER", "CMAKE_TOOLCHAIN_FILE"}
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            key = line.partition(":")[0]
            if key in wanted and "=" in line:
                result[key] = line.partition("=")[2]
    if sys.platform == "darwin":
        for key, command in (("xcode", ["xcodebuild", "-version"]),
                             ("sdk", ["xcrun", "--show-sdk-version"]),
                             ("compiler", ["xcrun", "clang", "--version"])):
            result[key] = subprocess.check_output(command, text=True).strip()
    return result


def collect(repo, build_dir, output_dir, version, arch):
    repo, build_dir, output_dir = (Path(value).resolve() for value in (repo, build_dir, output_dir))
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[.-][A-Za-z0-9]+)*", version):
        raise ValueError("Version must be a portable release version")
    if arch not in {"arm64", "x86_64"}:
        raise ValueError("Architecture must be arm64 or x86_64")
    if contained(repo, output_dir) or contained(repo, build_dir):
        raise ValueError("Build/output directories must not contain the repository")
    if run_git(repo, "status", "--porcelain", "--untracked-files=no").strip():
        raise ValueError("Tracked source changes would not match git archive HEAD")
    for name in run_git(repo, "ls-files", "--others", "--exclude-standard", "-z").decode().split("\0"):
        if name and not any(contained((repo / name).resolve(), root) for root in (build_dir, output_dir)):
            raise ValueError(f"Untracked source would not match git archive HEAD: {name}")
    commit = run_git(repo, "rev-parse", "HEAD").decode().strip()
    epoch = int(run_git(repo, "show", "-s", "--format=%ct", "HEAD").decode().strip())
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise ValueError("Invalid source commit")
    expected = pinned_sources(repo)
    sources = repo / "depends/sources"
    for name, digest in expected.items():
        path = sources / name
        if path.is_symlink() or not path.is_file() or sha256(path) != digest:
            raise ValueError(f"Missing or mismatched pinned depends source: {name}")
    mbedtls = mbedtls_source(build_dir)
    mbedtls_files = list(source_tree_files(mbedtls))
    archive_path = output_dir / f"connectcoin-{version}-macos-{arch}-sources.tar.gz"
    licenses_path = output_dir / f"licenses-{arch}"
    if any(path.exists() or path.is_symlink() for path in (archive_path, licenses_path)):
        raise ValueError("Source archive or licenses directory already exists; use a fresh output directory")
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".collect-sources-", dir=output_dir) as work:
        work = Path(work)
        notices = work / "licenses"
        notices.mkdir()
        inventory = []
        for name in REPO_NOTICES:
            copy_notice(notices, "connectcoin/" + name, (repo / name).read_bytes(), inventory)
        for name in sorted(expected):
            path = sources / name
            if tarfile.is_tarfile(path):
                archive_notices(path, notices, inventory)
        for path, name in mbedtls_files:
            if is_notice(name):
                data = path.read_bytes()
                if data:
                    copy_notice(notices, "mbedtls/" + name, data, inventory)
        instructions = rebuild_text(commit, version, arch)
        third_party = ("ConnectCoin Core third-party source and license notices\n\n"
                       "The application contains statically linked Qt (LGPLv3/GPL options), "
                       "QRencode (LGPL), ZeroMQ (MPL), and other third-party code. "
                       "ConnectCoin's MIT COPYING does not replace these upstream terms.\n"
                       "See LICENSE-MANIFEST.json for each notice's original path. The matching "
                       f"{archive_path.name} must accompany the DMG: it contains the complete "
                       "corresponding sources, recipe hashes, patches, and rebuild/relink instructions. "
                       "You may modify and relink the libraries; no restriction on reverse engineering "
                       "for debugging such modifications is imposed by this package.\n\n"
                       "Mbed TLS modifications include CMake compatibility, RSA-PSS restrictions, "
                       "X.509 verification, feature configuration, and an RSA-PSS adapter. Their "
                       "source and patch scripts are included. The upstream license texts and "
                       "copyright holders remain authoritative.\n")
        (notices / "COPYRIGHT.txt").write_bytes((repo / "COPYING").read_bytes())
        (notices / "THIRD-PARTY.txt").write_text(third_party, encoding="utf-8")
        (notices / "REBUILD.txt").write_text(instructions, encoding="utf-8")
        (notices / "LICENSE-MANIFEST.json").write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
        manifest = {"source_commit": commit, "version": version, "architecture": arch,
                    "depends_sha256": expected, "build_environment": build_environment(build_dir),
                    "mbedtls_files_sha256": {name: sha256(path) for path, name in mbedtls_files}}
        repo_tar = work / "repo.tar"
        subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", "--output", str(repo_tar), "HEAD"], check=True)
        staged_archive = work / archive_path.name
        with staged_archive.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as archive:
                with tarfile.open(repo_tar, "r:") as source:
                    for member in source:
                        check_member(member)
                        data = source.extractfile(member) if member.isfile() else None
                        member.name = "source/connectcoin/" + member.name
                        if member.islnk():
                            member.linkname = "source/connectcoin/" + member.linkname
                        member.uid = member.gid = 0
                        member.uname = member.gname = ""
                        member.mtime = epoch
                        archive.addfile(member, data)
                        if data:
                            data.close()
                for name in sorted(expected):
                    add_file(archive, "source/depends-sources/" + name, sources / name, epoch)
                for path, name in mbedtls_files:
                    add_file(archive, "source/mbedtls/" + name, path, epoch)
                for path in sorted(notices.iterdir()):
                    add_file(archive, "licenses/" + path.name, path, epoch)
                add_bytes(archive, "REBUILD.txt", instructions.encode(), epoch)
                add_bytes(archive, "build-environment.json", (json.dumps(manifest, indent=2) + "\n").encode(), epoch)
        staged_archive.rename(archive_path)
        try:
            notices.rename(licenses_path)
        except OSError:
            archive_path.unlink()  # Roll back only the artifact just created by this invocation.
            raise
    return {"source_archive": str(archive_path), "licenses_dir": str(licenses_path), "source_commit": commit}


class SourceTests(unittest.TestCase):
    @staticmethod
    def fixture(directory):
        root = Path(directory)
        repo, build, output = root / "repo", root / "build", root / "output"
        repo.mkdir()
        for name in REPO_NOTICES:
            path = repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("upstream copyright and license\n", encoding="utf-8")
        (repo / ".gitignore").write_text("depends/sources/\n", encoding="utf-8")
        subprocess.run(["git", "init", "--quiet", str(repo)], check=True)
        run_git(repo, "add", ".")
        run_git(repo, "-c", "user.name=Source Test", "-c", "user.email=test@example.invalid",
                "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Fixture")
        sources = repo / "depends/sources"
        sources.mkdir(parents=True)
        source = sources / "example.tar.gz"
        with tarfile.open(source, "w:gz") as archive:
            add_bytes(archive, "example/LICENSE", b"upstream license", 0)
            add_bytes(archive, "example/src.c", b"source", 0)
        mbed = build / "_deps/connectcoin_mbedtls-src"
        for name in ("LICENSE", "CMakeLists.txt", "include/mbedtls/version.h", "library/rsa.c"):
            path = mbed / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Mbed TLS source or license\n", encoding="utf-8")
        (mbed / ".git").write_text("private git pointer", encoding="utf-8")
        return repo, build, output, {source.name: sha256(source)}

    def test_complete_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            repo, build, output, expected = self.fixture(directory)
            with patch(__name__ + ".pinned_sources", return_value=expected):
                result = collect(repo, build, output, "1.0.0", "arm64")
                with self.assertRaises(ValueError):
                    collect(repo, build, output, "1.0.0", "arm64")
            with tarfile.open(result["source_archive"], "r:gz") as archive:
                names = archive.getnames()
                self.assertIn("source/connectcoin/COPYING", names)
                self.assertIn("source/depends-sources/example.tar.gz", names)
                self.assertIn("source/mbedtls/library/rsa.c", names)
                self.assertFalse(any(".git" in PurePosixPath(name).parts for name in names))
                with archive.extractfile("build-environment.json") as source:
                    manifest = json.load(source)
                self.assertEqual(manifest["depends_sha256"], expected)
                self.assertEqual(manifest["source_commit"], result["source_commit"])
            self.assertTrue((Path(result["licenses_dir"]) / "THIRD-PARTY.txt").is_file())

    def test_bad_hash_and_dirty_tree_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            repo, build, output, expected = self.fixture(directory)
            with patch(__name__ + ".pinned_sources", return_value={"example.tar.gz": "0" * 64}):
                with self.assertRaisesRegex(ValueError, "mismatched"):
                    collect(repo, build, output, "1.0.0", "x86_64")
            self.assertFalse(output.exists())
            (repo / "COPYING").write_text("changed", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Tracked source"):
                collect(repo, build, output, "1.0.0", "arm64")

    def test_untracked_source_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            repo, build, output, _ = self.fixture(directory)
            (repo / "new.cpp").write_text("source", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Untracked source"):
                collect(repo, build, output, "1.0.0", "arm64")

    def test_recipe_pins(self):
        pins = pinned_sources(Path(__file__).resolve().parents[2])
        self.assertEqual(len(pins), 10)
        self.assertIn("qtbase-everywhere-opensource-src-6.8.4.tar.xz", pins)

    def test_unsafe_names(self):
        for name in ("../escape", "/absolute", "C:/escape", "dir\\escape", "a/../../x", "a/\nfile", ""):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_name(name)

    def test_links_and_special_files(self):
        for link, kind in (("../../escape", tarfile.SYMTYPE), ("/root", tarfile.SYMTYPE),
                           ("other/file", tarfile.LNKTYPE)):
            item = tarfile.TarInfo("package/sub/link")
            item.type, item.linkname = kind, link
            with self.assertRaises(ValueError):
                check_member(item)
        item = tarfile.TarInfo("package/device")
        item.type = tarfile.CHRTYPE
        with self.assertRaises(ValueError):
            check_member(item)
        item.type, item.linkname = tarfile.SYMTYPE, "LICENSE"
        check_member(item)

    def test_notice_patterns(self):
        for name in ("qt/LICENSES/LGPL-3.0-only.txt", "qr/COPYING", "qt/src/3rdparty/thing/README",
                     "qt/src/3rdparty/thing/qt_attribution.json", "boost/LICENSE_1_0.txt"):
            self.assertTrue(is_notice(name), name)
        self.assertFalse(is_notice("qt/src/main.cpp"))

    def test_flat_notices_no_collision(self):
        with tempfile.TemporaryDirectory() as directory:
            inventory = []
            for origin in ("a/LICENSE", "b/LICENSE"):
                copy_notice(Path(directory), origin, b"license", inventory)
            self.assertEqual(len(list(Path(directory).iterdir())), 2)

    def test_tar_notices_and_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "source.tar"
            with tarfile.open(archive, "w") as output:
                add_bytes(output, "pkg/LICENSE", b"notice", 0)
            inventory = []
            archive_notices(archive, root, inventory)
            self.assertEqual(len(inventory), 1)
            with tarfile.open(archive, "w") as output:
                add_bytes(output, "../escape", b"bad", 0)
            with self.assertRaises(ValueError):
                archive_notices(archive, root, [])
            self.assertFalse((root.parent / "escape").exists())

    def test_duplicates_and_missing_notices(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "source.tar"
            for names in (("pkg/LICENSE", "pkg/LICENSE"), ("pkg/file.c",)):
                with tarfile.open(archive, "w") as output:
                    for name in names:
                        add_bytes(output, name, b"text", 0)
                with self.assertRaises(ValueError):
                    archive_notices(archive, root, [])

    def test_mbedtls_excludes_git(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".git").mkdir()
            (root / ".git/config").write_text("private", encoding="utf-8")
            (root / "LICENSE").write_text("license", encoding="utf-8")
            self.assertEqual([name for _, name in source_tree_files(root)], ["LICENSE"])

    def test_rebuild_metadata(self):
        result = rebuild_text("a" * 40, "1.0.0", "arm64")
        self.assertIn("Source commit: " + "a" * 40, result)
        self.assertIn("NO_IPC=1 NO_USDT=1", result)
        self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0", result)
        self.assertIn("FETCHCONTENT_SOURCE_DIR_CONNECTCOIN_MBEDTLS", result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--build-dir")
    parser.add_argument("--output-dir")
    parser.add_argument("--version")
    parser.add_argument("--arch", choices=("arm64", "x86_64"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SourceTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if not all((args.build_dir, args.output_dir, args.version, args.arch)):
        parser.error("--build-dir, --output-dir, --version and --arch are required")
    try:
        print(json.dumps(collect(args.repo, args.build_dir, args.output_dir, args.version, args.arch)))
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"Source collection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
