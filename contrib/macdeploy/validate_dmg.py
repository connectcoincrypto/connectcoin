#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Validate and smoke-test an ad-hoc macOS DMG without installing its application."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import plistlib
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


APP_NAME = "ConnectCoin Core.app"
EXECUTABLE = "Contents/MacOS/ConnectCoin-Qt"
MINIMUM_OS = "15.0"
MACHO_MAGIC = {bytes.fromhex(value) for value in (
    "feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca", "cafebabf", "bfbafeca",
)}
LOAD_DYLIB_COMMANDS = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                       "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}


class ValidationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ValidationError(message)


def run(command, *, environment=None, cwd=None, timeout=60):
    try:
        result = subprocess.run([str(arg) for arg in command], env=environment, cwd=cwd,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise ValidationError(f"Timed out after {timeout}s: {command[0]}") from error
    require(result.returncode == 0, f"{command[0]} exited {result.returncode}: {(result.stdout + result.stderr)[-4000:]}")
    return result


def inside(path, root):
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ValidationError(f"Broken or cyclic path: {path}") from error
    require(resolved.is_relative_to(root), f"Path escapes application: {path}")
    return resolved


def check_symlinks(app):
    require(app.is_dir() and not app.is_symlink(), f"Application must be a real directory: {app}")
    for directory, dirs, files in os.walk(app, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if path.is_symlink():
                inside(path, app)


def required_file(app, relative):
    path = inside(app / relative, app)
    require(path.is_file(), f"Required file is missing: {relative}")
    return path


def version_tuple(value):
    require(isinstance(value, str) and re.fullmatch(r"\d+(?:\.\d+){0,2}", value), f"Invalid version: {value!r}")
    return tuple(map(int, value.split("."))) + (0,) * (3 - len(value.split(".")))


def validate_plist(info, version):
    require(isinstance(info, dict), "Info.plist must be a dictionary")
    expected = {
        "CFBundleIdentifier": "com.connectcoincrypto.ConnectCoinCore",
        "CFBundleDisplayName": "ConnectCoin Core",
        "CFBundleExecutable": "ConnectCoin-Qt",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "CFBundleIconFile": "connectcoin.icns",
        "LSMinimumSystemVersion": MINIMUM_OS,
    }
    for key, value in expected.items():
        require(info.get(key) == value, f"Info.plist {key} must be {value!r}, got {info.get(key)!r}")
    urls = info.get("CFBundleURLTypes", [])
    require(isinstance(urls, list) and any(isinstance(item, dict) and
            isinstance(item.get("CFBundleURLSchemes"), list) and
            "connectcoin" in item["CFBundleURLSchemes"] for item in urls),
            "Info.plist does not register the connectcoin URL scheme")
    require("LSMinimumSystemVersionByArchitecture" not in info,
            "Unexpected per-architecture minimum OS override")


def validate_icon(data):
    require(len(data) >= 16 and data[:4] == b"icns" and int.from_bytes(data[4:8], "big") == len(data),
            "Invalid ICNS header or file length")
    offset = 8
    image_chunks = 0
    while offset < len(data):
        require(offset + 8 <= len(data), "Truncated ICNS chunk")
        size = int.from_bytes(data[offset + 4:offset + 8], "big")
        require(size > 8 and offset + size <= len(data), "Invalid ICNS chunk length")
        if data[offset:offset + 4] in {b"ic07", b"ic08", b"ic09", b"ic10", b"ic11", b"ic12", b"ic13", b"ic14"}:
            image_chunks += 1
        offset += size
    require(image_chunks > 0, "ICNS has no modern icon image")


def parse_load_commands(output):
    dependencies, rpaths, minimums, identities = [], [], [], []
    for block in re.split(r"(?m)^Load command \d+\s*$", output)[1:]:
        match = re.search(r"(?m)^\s*cmd (LC_\S+)\s*$", block)
        require(match is not None, "Malformed otool load command")
        kind = match[1]
        if kind in LOAD_DYLIB_COMMANDS | {"LC_ID_DYLIB", "LC_RPATH"}:
            field = "path" if kind == "LC_RPATH" else "name"
            value = re.search(rf"(?m)^\s*{field} (.+) \(offset \d+\)\s*$", block)
            require(value is not None, f"Malformed {kind}")
            (rpaths if kind == "LC_RPATH" else identities if kind == "LC_ID_DYLIB" else dependencies).append(value[1])
        elif kind in {"LC_BUILD_VERSION", "LC_VERSION_MIN_MACOSX"}:
            if kind == "LC_BUILD_VERSION":
                platform = re.search(r"(?m)^\s*platform (\S+)\s*$", block)
                require(platform is not None and platform[1] in {"1", "MACOS", "macos"}, "Non-macOS Mach-O platform")
            field = "minos" if kind == "LC_BUILD_VERSION" else "version"
            value = re.search(rf"(?m)^\s*{field} (\S+)\s*$", block)
            require(value is not None, f"Missing minimum OS in {kind}")
            version_tuple(value[1])
            minimums.append(value[1])
    require(minimums, "Mach-O has no macOS deployment target")
    return {"dependencies": dependencies, "rpaths": rpaths, "minimums": minimums, "identities": identities}


def is_system_library(name):
    return name.startswith("/") and posixpath.normpath(name).startswith(("/usr/lib/", "/System/Library/"))


def is_dynamic_qt(name):
    return re.search(r"(?:^|/)(?:Qt[^/]*\.framework|libQt[56][^/]*\.dylib|libq(?:cocoa|minimal|offscreen|macstyle)\.dylib)(?:/|$)", name) is not None


def expand_local(name, loader, executable, app):
    for token, base in (("@loader_path", loader.parent), ("@executable_path", executable.parent)):
        if name == token or name.startswith(token + "/"):
            return inside(base / name[len(token):].lstrip("/"), app)
    raise ValidationError(f"Non-relocatable or unsupported library path: {name}")


def resolve_dependency(name, loader, executable, app, rpaths):
    if is_system_library(name):
        return None
    if name.startswith("@rpath/"):
        tail = name[len("@rpath/"):]
        require(tail and ".." not in Path(tail).parts, f"Invalid @rpath dependency: {name}")
        for directory in rpaths:
            candidate = directory / tail
            if candidate.exists() or candidate.is_symlink():
                resolved = inside(candidate, app)
                require(resolved.is_file(), f"Dependency is not a file: {name}")
                return resolved
        raise ValidationError(f"Unresolved bundled dependency {name} in {loader.relative_to(app)}")
    resolved = expand_local(name, loader, executable, app)
    require(resolved.is_file(), f"Dependency is not a file: {name}")
    return resolved


def inspect_machos(app, arch, minimum_os):
    executable = required_file(app, EXECUTABLE)
    images = {}
    for directory, _, files in os.walk(app, followlinks=False):
        for name in files:
            path = inside(Path(directory) / name, app)
            if path in images or not path.is_file():
                continue
            with path.open("rb") as stream:
                if stream.read(4) not in MACHO_MAGIC:
                    continue
            architectures = run(["/usr/bin/lipo", "-archs", path]).stdout.split()
            require(architectures == [arch], f"Wrong architectures in {path.relative_to(app)}: {architectures}")
            metadata = parse_load_commands(run(["/usr/bin/otool", "-arch", arch, "-l", path]).stdout)
            require(all(version_tuple(value) <= version_tuple(minimum_os) for value in metadata["minimums"]),
                    f"Mach-O requires newer macOS than {minimum_os}: {path.relative_to(app)} {metadata['minimums']}")
            run(["/usr/bin/codesign", "--verify", "--strict", path])
            images[path] = metadata
    require(executable in images, f"Required component is not Mach-O: {EXECUTABLE}")
    executable_rpaths = images[executable]["rpaths"]
    for path, metadata in images.items():
        require(not is_dynamic_qt(path.as_posix()), f"Expected static Qt; found dynamic Qt image: {path.relative_to(app)}")
        local_rpaths = []
        # Reject build/Homebrew search paths even when all dependencies happen
        # to resolve elsewhere. System paths may not satisfy @rpath imports.
        for owner, paths in ((path, metadata["rpaths"]), (executable, executable_rpaths)):
            for entry in paths:
                if is_system_library(entry):
                    continue
                directory = expand_local(entry, owner, executable, app)
                require(directory.is_dir(), f"RPATH is not a directory: {entry}")
                local_rpaths.append(directory)
        for identity in metadata["identities"]:
            require(identity.startswith(("@rpath/", "@loader_path/", "@executable_path/")),
                    f"Non-relocatable library identity: {identity}")
        for name in metadata["dependencies"]:
            require(not is_dynamic_qt(name), f"Expected static Qt; found dynamic Qt dependency: {name}")
            dependency = resolve_dependency(name, path, executable, app, local_rpaths)
            require(dependency is None or dependency in images, f"Dependency is not inspected Mach-O: {name}")
    return [{"path": str(path.relative_to(app)), "architecture": arch,
             "minimum_os": metadata["minimums"], "dependencies": metadata["dependencies"],
             "rpaths": metadata["rpaths"]} for path, metadata in sorted(images.items())]


def validate_app(app, arch, version):
    check_symlinks(app)
    with required_file(app, "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    validate_plist(info, version)
    validate_icon(required_file(app, "Contents/Resources/connectcoin.icns").read_bytes())
    require(os.access(required_file(app, EXECUTABLE), os.X_OK), "Application binary is not executable")
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", app])
    signature = run(["/usr/bin/codesign", "--display", "--verbose=4", app]).stderr
    require(re.search(r"(?m)^Signature=adhoc\s*$", signature), "Expected an ad-hoc signature")
    require("Authority=" not in signature, "Unexpected certificate authority in ad-hoc bundle")
    return inspect_machos(app, arch, info["LSMinimumSystemVersion"])


def isolated_environment(root):
    environment = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C", "LC_ALL": "C",
                   "QT_QPA_PLATFORM": "cocoa", "QT_DEBUG_PLUGINS": "1", "QT_FORCE_STDERR_LOGGING": "1",
                   "DYLD_PRINT_LIBRARIES": "1"}
    for variable, name in (("HOME", "home"), ("TMPDIR", "tmp"), ("XDG_CONFIG_HOME", "config"),
                           ("XDG_CACHE_HOME", "cache"), ("XDG_DATA_HOME", "data")):
        directory = root / name
        directory.mkdir(mode=0o700)
        environment[variable] = str(directory)
    # CFPreferences on macOS needs this override in addition to HOME.
    environment["CFFIXED_USER_HOME"] = environment["HOME"]
    return environment


def validate_loaded_libraries(diagnostics, app):
    loaded = []
    for line in diagnostics.splitlines():
        if not re.match(r"^dyld(?:\[\d+\])?:", line):
            continue
        match = re.search(r"(/[\S ].*)$", line)
        if match is None:
            continue
        name = match[1].strip()
        require(not is_dynamic_qt(name), f"Static Qt smoke unexpectedly loaded a dynamic Qt library: {name}")
        if not is_system_library(name):
            inside(Path(name), app)
        loaded.append(name)
    require(loaded, "DYLD_PRINT_LIBRARIES produced no loaded-library evidence")
    return len(loaded)


def smoke_app(app, root, version):
    environment = isolated_environment(root)
    datadir = root / "regtest-data"
    datadir.mkdir(mode=0o700)
    reports = []
    for option in ("--version", "--help"):
        result = run([app / EXECUTABLE, option, "-lang=en", f"-datadir={datadir}", "-noconf", "-nosettings",
                      "-regtest", "-noconnect", "-listen=0", "-dnsseed=0", "-discover=0"],
                     environment=environment, cwd=root, timeout=45)
        require(re.search(rf"ConnectCoin Core.*?version v?{re.escape(version)}(?:\b|[-.])", result.stdout),
                f"Unexpected English {option} output: {result.stdout[:300]!r}")
        if option == "--help":
            require("Usage:" in result.stdout and "Options:" in result.stdout, "Missing English help sections")
        loaded_count = validate_loaded_libraries(result.stderr, app)
        require(not any((datadir / name).exists() for name in ("blocks", "chainstate", "regtest", "wallets")),
                "Help/version unexpectedly initialized node data")
        # QApplication is constructed before the help/version early return.
        # Successful initialization with QT_QPA_PLATFORM=cocoa exercises the
        # statically imported Cocoa plugin; static plugins have no dylib log.
        reports.append({"option": option, "exit_code": 0, "static_cocoa_startup": True,
                        "loaded_libraries_checked": loaded_count,
                        "first_line": result.stdout.splitlines()[0]})
    return reports


def validate_volume(volume):
    app = volume / APP_NAME
    require(app.is_dir() and not app.is_symlink(), f"DMG lacks {APP_NAME}")
    applications = volume / "Applications"
    require(applications.is_symlink() and os.readlink(applications) == "/Applications",
            "DMG Applications link must point exactly to /Applications")
    instructions = volume / "INSTALL.txt"
    require(instructions.is_file() and not instructions.is_symlink(), "DMG lacks a regular INSTALL.txt")
    text = instructions.read_text(encoding="utf-8")
    require("ConnectCoin Core" in text and "Applications" in text and "not notarized" in text.lower(),
            "INSTALL.txt must explain installation and the non-notarized distribution in English")
    check_symlinks(app)
    return app


@contextmanager
def mounted_dmg(dmg):
    root = Path(tempfile.mkdtemp(prefix="connectcoin-dmg-validation-")).resolve()
    parent = root.parent
    volume = root / "mounted"
    volume.mkdir()
    attached = False
    try:
        run(["/usr/bin/hdiutil", "verify", dmg], timeout=120)
        result = run(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen", "-plist",
                      "-mountpoint", volume, dmg], timeout=120)
        attached = True
        metadata = plistlib.loads(result.stdout.encode("utf-8"))
        require(any(item.get("mount-point") == str(volume) for item in metadata.get("system-entities", [])),
                "hdiutil did not mount the image at the owned mountpoint")
        yield root, volume
    finally:
        if attached or os.path.ismount(volume):
            # Never detach by a guessed device or a user-owned volume name.
            for attempt in range(3):
                try:
                    run(["/usr/bin/hdiutil", "detach", volume], timeout=30)
                    break
                except ValidationError:
                    if attempt == 2:
                        raise ValidationError(f"Could not detach owned mountpoint; preserved temporary directory: {root}")
                    time.sleep(1)
        require(not os.path.ismount(volume), f"Refusing cleanup of a mounted volume: {volume}")
        require(root.resolve() == root and root.parent == parent and root.name.startswith("connectcoin-dmg-validation-"),
                f"Refusing cleanup of unexpected temporary path: {root}")
        shutil.rmtree(root)


def validate_dmg(dmg, arch, version):
    require(sys.platform == "darwin", "Real DMG validation must run on macOS; use --self-test elsewhere")
    require(dmg.is_file() and not dmg.is_symlink(), "DMG must be a regular, non-symlink file")
    with mounted_dmg(dmg) as (root, volume):
        source_app = validate_volume(volume)
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", source_app])
        copied = root / "relocated copy with spaces" / APP_NAME
        copied.parent.mkdir()
        run(["/usr/bin/ditto", source_app, copied], timeout=120)
        machos = validate_app(copied, arch, version)
        smoke = smoke_app(copied, root, version)
    with dmg.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"dmg": dmg.name, "sha256": digest.hexdigest(), "architecture": arch, "version": version,
            "minimum_macos": MINIMUM_OS, "signature": "ad-hoc", "developer_id_signed": False,
            "notarized": False, "gatekeeper_acceptance_tested": False, "mach_o_images": machos,
            "commands": smoke, "readonly_mount": True, "relocated_copy_tested": True,
            "temporary_profile_removed": True, "owned_volume_detached": True,
            "qt_linkage": "static", "gui_coverage": "English help/version with statically imported Cocoa; no interactive GUI or node startup."}


class RegressionTests(unittest.TestCase):
    @staticmethod
    def sample_plist():
        return {"CFBundleIdentifier": "com.connectcoincrypto.ConnectCoinCore", "CFBundleDisplayName": "ConnectCoin Core",
                "CFBundleExecutable": "ConnectCoin-Qt", "CFBundlePackageType": "APPL", "CFBundleVersion": "1.0.0",
                "CFBundleShortVersionString": "1.0.0", "CFBundleIconFile": "connectcoin.icns",
                "LSMinimumSystemVersion": "15.0", "CFBundleURLTypes": [{"CFBundleURLSchemes": ["connectcoin"]}]}

    @classmethod
    def app_fixture(cls, app):
        for name in (EXECUTABLE, "Contents/Frameworks/library.dylib"):
            path = app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(bytes.fromhex("cffaedfe") + b"fixture")
            path.chmod(0o755)
        resources = app / "Contents/Resources"
        resources.mkdir()
        (resources / "connectcoin.icns").write_bytes(b"icns" + (20).to_bytes(4, "big") + b"ic10" + (12).to_bytes(4, "big") + b"data")
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(cls.sample_plist()))

    @staticmethod
    def native_tool_fixture(command, **_kwargs):
        stdout, stderr = "", ""
        tool = Path(command[0]).name
        if tool == "lipo":
            stdout = "arm64\n"
        elif tool == "otool":
            stdout = "Load command 0\n cmd LC_BUILD_VERSION\n platform 1\n minos 15.0\n"
            if Path(command[-1]).name == "ConnectCoin-Qt":
                stdout += "Load command 1\n cmd LC_RPATH\n path @executable_path/../Frameworks (offset 12)\n"
                stdout += "Load command 2\n cmd LC_LOAD_DYLIB\n name @rpath/library.dylib (offset 24)\n"
        elif tool == "codesign" and "--display" in command:
            stderr = "Signature=adhoc\nTeamIdentifier=not set\n"
        return subprocess.CompletedProcess(command, 0, stdout, stderr)

    def test_plist_contract(self):
        validate_plist(self.sample_plist(), "1.0.0")
        for key, value in (("CFBundleVersion", "0.9"), ("CFBundleIdentifier", "org.bitcoin.Bitcoin-Qt"),
                           ("CFBundleExecutable", "../evil"), ("LSMinimumSystemVersion", "14.0"),
                           ("LSMinimumSystemVersionByArchitecture", {"arm64": "16.0"}),
                           ("CFBundleURLTypes", [{"CFBundleURLSchemes": "not-connectcoin"}])):
            with self.subTest(key=key), self.assertRaises(ValidationError):
                validate_plist({**self.sample_plist(), key: value}, "1.0.0")
        with self.assertRaises(ValidationError):
            validate_plist([], "1.0.0")

    def test_app_graph_and_signature(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary).resolve() / APP_NAME
            self.app_fixture(app)
            with patch(__name__ + ".run", side_effect=self.native_tool_fixture) as tool:
                images = validate_app(app, "arm64", "1.0.0")
            self.assertEqual(len(images), 2)
            self.assertTrue(any("--deep" in call.args[0] and "--strict" in call.args[0] for call in tool.call_args_list))

    def test_rejects_architecture_newer_os_external_paths_and_bad_signature(self):
        changes = (("arm64\n", "x86_64\n"), ("minos 15.0", "minos 15.1"),
                   ("@executable_path/../Frameworks", "/opt/homebrew/lib"),
                   ("@rpath/library.dylib", "/usr/local/opt/library/lib/library.dylib"),
                   ("@rpath/library.dylib", "@rpath/QtCore.framework/QtCore"),
                   ("Signature=adhoc", "Authority=Developer ID Application: unexpected"))
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary).resolve() / APP_NAME
            self.app_fixture(app)
            for old, new in changes:
                def tool(command, **kwargs):
                    result = self.native_tool_fixture(command, **kwargs)
                    result.stdout = result.stdout.replace(old, new)
                    result.stderr = result.stderr.replace(old, new)
                    return result
                with self.subTest(change=new), patch(__name__ + ".run", side_effect=tool), self.assertRaises(ValidationError):
                    validate_app(app, "arm64", "1.0.0")

    def test_smoke_requires_static_cocoa_and_english_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            app = root / APP_NAME
            result = subprocess.CompletedProcess([], 0, "ConnectCoin Core version v1.0.0\nUsage:\nOptions:\n",
                                                 'dyld[123]: <0000> /usr/lib/libSystem.B.dylib\n')
            with patch(__name__ + ".run", return_value=result) as tool:
                self.assertEqual(len(smoke_app(app, root, "1.0.0")), 2)
            for call in tool.call_args_list:
                self.assertIn("-regtest", call.args[0])
                self.assertIn("-noconf", call.args[0])
                self.assertIn("-nosettings", call.args[0])
                self.assertEqual(call.kwargs["timeout"], 45)
                self.assertEqual(call.kwargs["environment"]["QT_QPA_PLATFORM"], "cocoa")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            result.stderr = 'dyld[123]: <0000> /opt/homebrew/plugins/libqcocoa.dylib\n'
            with patch(__name__ + ".run", return_value=result), self.assertRaisesRegex(ValidationError, "dynamic Qt"):
                smoke_app(root / APP_NAME, root, "1.0.0")

    def test_loaded_library_paths_are_confined(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary).resolve() / APP_NAME
            self.app_fixture(app)
            # dyld always prints POSIX paths, including when this parser's
            # regression tests run on a Windows development machine.
            with patch(__name__ + ".inside", return_value=app / EXECUTABLE) as confine:
                self.assertEqual(validate_loaded_libraries("dyld[123]: <uuid> /owned/app/binary\n", app), 1)
            confine.assert_called_once_with(Path("/owned/app/binary"), app)
            for text in ("", "dyld[123]: <uuid> /tmp/outside-the-app\n", "dyld[123]: /opt/homebrew/lib/library.dylib\n"):
                with self.subTest(text=text), self.assertRaises(ValidationError):
                    validate_loaded_libraries(text, app)

    def test_owned_mount_is_readonly_and_detached_on_validation_failure(self):
        calls = []
        def tool(command, **_kwargs):
            calls.append(command)
            output = ""
            if "attach" in command:
                output = plistlib.dumps({"system-entities": [{"mount-point": str(command[command.index("-mountpoint") + 1])}]}).decode()
            return subprocess.CompletedProcess(command, 0, output, "")
        with patch(__name__ + ".run", side_effect=tool):
            with self.assertRaisesRegex(ValidationError, "fixture failure"):
                with mounted_dmg(Path("fixture.dmg")) as (root, volume):
                    self.assertTrue(root.exists())
                    raise ValidationError("fixture failure")
        self.assertFalse(root.exists())
        attach = next(command for command in calls if "attach" in command)
        self.assertIn("-readonly", attach)
        self.assertIn("-nobrowse", attach)
        self.assertEqual(calls[-1], ["/usr/bin/hdiutil", "detach", volume])

    def test_failed_verify_never_attaches_or_runs_app(self):
        with patch(__name__ + ".run", side_effect=ValidationError("corrupt image")) as tool:
            with self.assertRaisesRegex(ValidationError, "corrupt image"):
                with mounted_dmg(Path("fixture.dmg")):
                    self.fail("Corrupt image was mounted")
        self.assertEqual(tool.call_count, 1)
        self.assertEqual(tool.call_args.args[0][:2], ["/usr/bin/hdiutil", "verify"])

    def test_failed_detach_preserves_owned_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / "connectcoin-dmg-validation-fixture"
            root.mkdir()
            def tool(command, **_kwargs):
                if "detach" in command:
                    raise ValidationError("busy owned image")
                output = plistlib.dumps({"system-entities": [{"mount-point": str(root / "mounted")}]}).decode()
                return subprocess.CompletedProcess(command, 0, output, "")
            with patch.object(tempfile, "mkdtemp", return_value=str(root)), patch(__name__ + ".run", side_effect=tool) as commands, patch.object(time, "sleep"):
                with self.assertRaisesRegex(ValidationError, "preserved temporary directory"):
                    with mounted_dmg(Path("fixture.dmg")):
                        pass
            self.assertTrue(root.is_dir())
            self.assertEqual(sum("detach" in call.args[0] for call in commands.call_args_list), 3)

    def test_volume_layout_and_instructions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            app = root / APP_NAME
            app.mkdir()
            (root / "Applications").touch()
            instructions = root / "INSTALL.txt"
            instructions.write_text("Drag ConnectCoin Core to Applications. This app is not notarized.\n", encoding="utf-8")
            with patch.object(Path, "is_symlink", side_effect=lambda: False), self.assertRaises(ValidationError):
                validate_volume(root)
            # Mock just the Applications link so the full layout test also runs
            # on Windows without symlink-creation privileges.
            with patch.object(Path, "is_symlink", autospec=True, side_effect=lambda path: path.name == "Applications"), patch.object(os, "readlink", return_value="/Applications"):
                self.assertEqual(validate_volume(root), app)
                instructions.write_text("unrelated text", encoding="utf-8")
                with self.assertRaisesRegex(ValidationError, "INSTALL.txt"):
                    validate_volume(root)

    def test_load_commands(self):
        text = """example:
Load command 0
          cmd LC_BUILD_VERSION
      cmdsize 32
     platform 1
         minos 15.0
           sdk 26.0
Load command 1
          cmd LC_LOAD_DYLIB
         name @rpath/QtCore.framework/Versions/A/QtCore (offset 24)
Load command 2
          cmd LC_RPATH
         path @executable_path/../Frameworks (offset 12)
"""
        value = parse_load_commands(text)
        self.assertEqual(value["minimums"], ["15.0"])
        self.assertEqual(value["rpaths"], ["@executable_path/../Frameworks"])
        self.assertEqual(len(value["dependencies"]), 1)
        with self.assertRaises(ValidationError):
            parse_load_commands(text.replace("platform 1", "platform 2"))
        with self.assertRaises(ValidationError):
            parse_load_commands("no deployment target")
        with self.assertRaises(ValidationError):
            parse_load_commands(text.replace("(offset 24)", "broken"))

    def test_legacy_deployment_target(self):
        metadata = parse_load_commands("Load command 0\n cmd LC_VERSION_MIN_MACOSX\n version 14.0\n sdk 15.0\n")
        self.assertEqual(metadata["minimums"], ["14.0"])
        self.assertLess(version_tuple("14.9"), version_tuple("15.0"))
        self.assertGreater(version_tuple("15.1"), version_tuple("15.0"))
        with self.assertRaises(ValidationError):
            version_tuple("15.x")

    def test_system_libraries(self):
        for path in ("/usr/lib/libSystem.B.dylib", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"):
            self.assertTrue(is_system_library(path))
        for path in ("/opt/homebrew/lib/libQt6Core.dylib", "/usr/local/lib/libQt6Core.dylib",
                     "/usr/lib/../../tmp/evil.dylib", "/System/Library/../../tmp/evil.dylib", "relative.dylib"):
            self.assertFalse(is_system_library(path))

    def test_icon_validation(self):
        chunk = b"ic10" + (12).to_bytes(4, "big") + b"data"
        valid = b"icns" + (8 + len(chunk)).to_bytes(4, "big") + chunk
        validate_icon(valid)
        for invalid in (b"", valid[:-1], valid.replace(b"ic10", b"zzzz"), valid[:12] + b"\0\0\0\0" + valid[16:]):
            with self.subTest(invalid=invalid), self.assertRaises(ValidationError):
                validate_icon(invalid)

    def test_environment_does_not_inherit_host_plugins_or_preferences(self):
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {"DYLD_LIBRARY_PATH": "/opt/homebrew/lib", "QT_PLUGIN_PATH": "/tmp/plugins"}):
            environment = isolated_environment(Path(temporary))
            self.assertNotIn("DYLD_LIBRARY_PATH", environment)
            self.assertNotIn("QT_PLUGIN_PATH", environment)
            self.assertEqual(environment["HOME"], environment["CFFIXED_USER_HOME"])
            self.assertEqual(environment["QT_QPA_PLATFORM"], "cocoa")
            self.assertNotIn("homebrew", environment["PATH"])

    def test_dependency_resolution(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary).resolve()
            executable = app / EXECUTABLE
            executable.parent.mkdir(parents=True)
            executable.touch()
            frameworks = app / "Contents/Frameworks"
            frameworks.mkdir()
            library = frameworks / "library.dylib"
            library.touch()
            self.assertEqual(expand_local("@executable_path/../Frameworks", library, executable, app), frameworks)
            self.assertEqual(resolve_dependency("@rpath/library.dylib", executable, executable, app, [frameworks]), library)
            self.assertEqual(resolve_dependency("@loader_path/library.dylib", library, executable, app, []), library)
            self.assertIsNone(resolve_dependency("/usr/lib/libSystem.B.dylib", executable, executable, app, []))
            for name in ("/opt/homebrew/lib/library.dylib", "library.dylib", "@rpath/missing.dylib", "@rpath/../library.dylib"):
                with self.subTest(name=name), self.assertRaises(ValidationError):
                    resolve_dependency(name, executable, executable, app, [frameworks])

    def test_symlink_escape_and_internal_framework_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            app = root / APP_NAME
            app.mkdir()
            (app / "real").touch()
            try:
                (app / "link").symlink_to("real")
            except OSError as error:
                self.skipTest(f"Symlinks unavailable: {error}")
            check_symlinks(app)
            (app / "escape").symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValidationError):
                check_symlinks(app)

    def test_timeout_is_a_validation_failure(self):
        with patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired("owned-test", 1)):
            with self.assertRaisesRegex(ValidationError, "Timed out"):
                run(["owned-test"], timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dmg", type=Path)
    parser.add_argument("--arch", choices=("arm64", "x86_64"))
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(RegressionTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if options.dmg is None or options.arch is None:
        parser.error("Specify --dmg and --arch, or use --self-test")
    version_tuple(options.version)
    if options.output is not None:
        require(not options.output.exists() and not options.output.is_symlink(), "Refusing to overwrite an existing verification report")
        require(options.output.resolve() != options.dmg.resolve(), "Verification report must not overwrite the DMG")
    report = validate_dmg(options.dmg.absolute(), options.arch, options.version)
    if options.output is not None:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        with options.output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {report['dmg']}: {len(report['mach_o_images'])} Mach-O images; ad-hoc signature; relocated Cocoa help/version")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValidationError, OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
