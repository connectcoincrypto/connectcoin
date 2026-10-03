#!/usr/bin/env python3
# Copyright (c) 2026 The ConnectCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Verify an installed Linux prefix or portable bundle without using user data."""

import argparse
import configparser
import gzip
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
import zlib


TARGETS = ("connectcoin", "connectcoind", "connectcoin-cli", "connectcoin-tx",
           "connectcoin-wallet", "connectcoin-util", "connectcoin-qt")
DESKTOP_FILE = "share/applications/org.connectcoin.ConnectCoin.desktop"
ICON_FILE = "share/icons/hicolor/1024x1024/apps/connectcoin.png"
FALLBACK_ICON = "share/pixmaps/connectcoin.png"
MAX_MANUAL_BYTES = 1024 * 1024
STARTUP_TIMEOUT = 120
RPC_TIMEOUT = 30
STOP_TIMEOUT = 30


class SmokeError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise SmokeError(message)


def installed_file(prefix, relative):
    path = prefix / relative
    resolved = path.resolve(strict=True)
    require(resolved.is_relative_to(prefix) and resolved.is_file(),
            f"Installed file escapes the prefix or is not a file: {relative}")
    # AppRun selects the portable command using the invoked symlink's name.
    return path


def validate_manual(prefix, relative, target):
    # RPM may compress man pages after staging. Validate every installed form
    # so an old uncompressed placeholder cannot hide behind a valid .gz page.
    candidates = [name for name in (relative, relative + ".gz")
                  if (prefix / name).exists() or (prefix / name).is_symlink()]
    require(candidates, f"Missing manual page: {relative} (or .gz)")
    for name in candidates:
        path = installed_file(prefix, name)
        try:
            opener = gzip.open if name.endswith(".gz") else open
            with opener(path, "rb") as manual:
                data = manual.read(MAX_MANUAL_BYTES + 1)
            require(len(data) <= MAX_MANUAL_BYTES, f"Manual page exceeds 1 MiB: {name}")
            text = data.decode("utf-8")
        except (OSError, EOFError, UnicodeError, zlib.error) as error:
            raise SmokeError(f"Unreadable manual page: {name}") from error
        require("placeholder" not in text.casefold(), f"Placeholder manual page: {name}")
        # help2man titles may quote the command or escape its hyphens.
        header = re.search(r'^\.TH[ \t]+(?:"([^"]+)"|(\S+))', text.replace("\\-", "-"), re.MULTILINE | re.IGNORECASE)
        require(header is not None and (header[1] or header[2]).casefold() == target.casefold(),
                f"Manual page must have a .TH header for {target}: {name}")


def validate_layout(prefix, portable=False):
    require(prefix.is_dir(), f"Not an installation prefix: {prefix}")
    binaries = {target: installed_file(prefix, target if portable else f"bin/{target}") for target in TARGETS}
    for target, binary in binaries.items():
        require(os.access(binary, os.X_OK), f"Not executable: {target}")
    data_prefix = "usr/" if portable else ""
    desktop = installed_file(prefix, data_prefix + DESKTOP_FILE)
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(desktop.read_text(encoding="utf-8"))
    entry = parser["Desktop Entry"]
    require(entry.get("Type") == "Application" and entry.get("Icon") == "connectcoin",
            "Desktop entry must identify the ConnectCoin application and icon")
    command = shlex.split(entry.get("Exec", ""))
    require(command and command[0] == "connectcoin-qt", "Desktop entry must launch connectcoin-qt")
    for name in (ICON_FILE, FALLBACK_ICON):
        with installed_file(prefix, data_prefix + name).open("rb") as icon:
            require(icon.read(8) == b"\x89PNG\r\n\x1a\n", f"Not a PNG icon: {name}")
    for target in TARGETS:
        validate_manual(prefix, f"{data_prefix}share/man/man1/{target}.1", target)
    return binaries


def isolated_environment(root):
    environment = dict(os.environ)
    # Do not resolve libraries, Qt plugins, or preferences from a developer's
    # environment. Portable binaries must resolve their own bundled libraries.
    for key in tuple(environment):
        if key.startswith(("LD_", "QT_")) or key in {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"}:
            del environment[key]
    directories = {
        "HOME": "home", "XDG_CONFIG_HOME": "config", "XDG_DATA_HOME": "data",
        "XDG_CACHE_HOME": "cache", "XDG_STATE_HOME": "state", "XDG_RUNTIME_DIR": "runtime",
        "TMPDIR": "tmp",
    }
    for variable, name in directories.items():
        directory = root / name
        directory.mkdir(mode=0o700)
        environment[variable] = str(directory)
    environment.update({
        "LC_ALL": "C", "LANG": "C", "PATH": "/usr/bin:/bin",
        "XDG_CONFIG_DIRS": str(root / "config"),
        "DBUS_SESSION_BUS_ADDRESS": f"unix:path={root / 'no-session-bus'}",
        "QT_QPA_PLATFORM": "minimal", "QT_STYLE_OVERRIDE": "fusion",
        "QT_FORCE_STDERR_LOGGING": "1",
    })
    return environment


def run(command, environment, directory, timeout=RPC_TIMEOUT):
    try:
        result = subprocess.run([str(arg) for arg in command], cwd=directory, env=environment,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise SmokeError(f"Timed out after {timeout}s: {Path(command[0]).name}") from error
    require(result.returncode == 0,
            f"{Path(command[0]).name} exited {result.returncode}: "
            f"{(result.stdout + result.stderr)[-2000:]}")
    return result.stdout


def validate_portable_dependencies(prefix, environment, root):
    """Check ELF applications/plugins, not shell helpers such as Huge Pages."""
    libraries = (prefix / "usr/lib").resolve(strict=True)
    plugins = (prefix / "usr/plugins").resolve(strict=True)
    for directory in (libraries, plugins):
        require(directory.is_relative_to(prefix) and directory.is_dir(),
                f"Portable runtime directory escapes the bundle: {directory}")
    for plugin in ("libqxcb.so", "libqminimal.so"):
        installed_file(prefix, f"usr/plugins/platforms/{plugin}")
    # Enumerate the required native programs explicitly. The bin directory also
    # contains connectcoin-hugepages (a shell script) and Qt configuration files.
    relatives = [f"usr/bin/{target}" for target in TARGETS]
    relatives += [path.relative_to(prefix).as_posix() for path in sorted(plugins.rglob("*.so"))]
    loader_environment = dict(environment)
    loader_environment["LD_LIBRARY_PATH"] = str(libraries)
    errors = []
    for relative in relatives:
        try:
            binary = installed_file(prefix, relative)
            with binary.open("rb") as stream:
                require(stream.read(4) == b"\x7fELF", "Expected an ELF executable or shared library")
            output = run(["ldd", binary], loader_environment, root)
            require("not found" not in output, f"Missing runtime library:\n{output[-2000:]}")
        except (SmokeError, OSError) as error:
            errors.append(f"{relative}: {error}")
    require(not errors, "Portable dependency validation failed:\n" + "\n".join(errors))
    return relatives


def version_from_output(output):
    first_line = output.strip().splitlines()[0] if output.strip() else ""
    match = re.fullmatch(r"ConnectCoin Core .*?version (v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?)(?:\s.*)?", first_line)
    require(match is not None, f"Unexpected version output: {first_line!r}")
    return match[1]


def smoke_help(binaries, environment, root):
    datadir = root / "help-datadir"
    datadir.mkdir()
    reports = []
    versions = set()
    for target in TARGETS:
        for option in ("--version", "--help"):
            command = [binaries[target], option]
            if target in {"connectcoind", "connectcoin-qt"}:
                command += [f"-datadir={datadir}", "-noconf", "-nosettings"]
            if target == "connectcoin-qt":
                command.append("-lang=en")
            output = run(command, environment, root)
            require("connectcoin" in output.lower(), f"Empty/unexpected {target} {option} output")
            report = {"executable": target, "option": option, "exit_code": 0}
            if option == "--version":
                report["version"] = version_from_output(output)
                versions.add(report["version"])
            reports.append(report)
    require(len(versions) == 1, f"Installed executables have different versions: {sorted(versions)}")
    return reports


def daemon_arguments(binary, datadir, cookie, port):
    return [binary, "-regtest", f"-datadir={datadir}", "-noconf", "-nosettings",
            "-server=1", "-daemon=0", "-networkactive=0", "-listen=0", "-listenonion=0",
            "-dns=0", "-dnsseed=0", "-fixedseeds=0", "-discover=0", "-natpmp=0", "-noconnect",
            "-randomxfast=0", "-keypool=10", "-printtoconsole=1", "-nodebuglogfile",
            "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1", f"-rpcport={port}",
            f"-rpccookiefile={cookie}"]


def stop_owned_process(process, rpc):
    if process.poll() is not None:
        return
    try:
        rpc("stop", timeout=5)
        process.wait(timeout=STOP_TIMEOUT)
        return
    except (OSError, SmokeError, subprocess.TimeoutExpired):
        pass
    # Only signal the child started by this test, never an existing node.
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def smoke_regtest(binaries, environment, root):
    datadir = root / "regtest-datadir"
    datadir.mkdir()
    cookie = root / "rpc.cookie"
    # The unique cookie also prevents accidentally talking to an unrelated node
    # if another process claims the selected port before our child binds it.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    base = [binaries["connectcoin-cli"], "-regtest", f"-datadir={datadir}", "-noconf",
            "-rpcconnect=127.0.0.1", f"-rpcport={port}", f"-rpccookiefile={cookie}",
            f"-rpcclienttimeout={RPC_TIMEOUT}"]

    def rpc(*arguments, timeout=RPC_TIMEOUT):
        return run([*base, *arguments], environment, root, timeout=timeout).strip()

    started = time.monotonic()
    log_path = root / "daemon.log"
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(daemon_arguments(binaries["connectcoind"], datadir, cookie, port),
                                   cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = started + STARTUP_TIMEOUT
            last_error = "RPC has not become ready"
            while time.monotonic() < deadline:
                require(process.poll() is None, f"Daemon exited before RPC startup: {process.returncode}")
                if cookie.is_file():
                    try:
                        blockchain = json.loads(rpc("getblockchaininfo", timeout=3))
                        break
                    except (SmokeError, ValueError) as error:
                        last_error = str(error)
                time.sleep(0.25)
            else:
                raise SmokeError(f"Regtest startup timed out: {last_error}")
            startup_seconds = round(time.monotonic() - started, 3)
            require(blockchain["chain"] == "regtest" and blockchain["blocks"] == 0,
                    "Daemon did not start on an empty isolated regtest chain")
            network = json.loads(rpc("getnetworkinfo"))
            require(network["networkactive"] is False and network["connections"] == 0,
                    "Smoke daemon unexpectedly enabled P2P connections")
            wallet = json.loads(rpc("createwallet", "linux-package-smoke"))
            require(wallet["name"] == "linux-package-smoke", "Unexpected created wallet")
            address = rpc("-rpcwallet=linux-package-smoke", "getnewaddress")
            require(address and not any(character.isspace() for character in address),
                    "Wallet did not generate an address")
            address_info = json.loads(rpc("-rpcwallet=linux-package-smoke", "getaddressinfo", address))
            require(address_info["ismine"] is True, "Generated address is not owned by the temporary wallet")
            rpc("stop")
            require(process.wait(timeout=STOP_TIMEOUT) == 0, "Daemon shutdown failed")
            return {"chain": "regtest", "blocks": 0, "connections": 0,
                    "wallet_created": True, "owned_address_created": True,
                    "startup_seconds": startup_seconds, "graceful_shutdown": True}
        except (SmokeError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            diagnostics = log_path.read_text(encoding="utf-8", errors="replace")[-4000:]
            raise SmokeError(f"{error}\nDaemon log tail:\n{diagnostics}") from error
        finally:
            stop_owned_process(process, rpc)


class RegressionTests(unittest.TestCase):
    def dependency_fixture(self, root):
        (root / "usr/bin").mkdir(parents=True)
        (root / "usr/lib").mkdir()
        (root / "usr/plugins/platforms").mkdir(parents=True)
        relatives = [f"usr/bin/{target}" for target in TARGETS]
        relatives += ["usr/plugins/platforms/libqminimal.so", "usr/plugins/platforms/libqxcb.so"]
        for relative in relatives:
            (root / relative).write_bytes(b"\x7fELFfixture")
        (root / "usr/bin/connectcoin-hugepages").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        (root / "usr/bin/qt.conf").write_text("[Paths]\n", encoding="utf-8")
        return relatives

    def test_portable_dependencies_skip_scripts_but_check_all_native_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            relatives = self.dependency_fixture(root)
            environment = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
            with patch(__name__ + ".run", return_value="libc.so.6 => /lib/libc.so.6") as command:
                self.assertEqual(validate_portable_dependencies(root, environment, root), relatives)
            self.assertEqual(command.call_count, len(relatives))
            for call, relative in zip(command.call_args_list, relatives):
                self.assertEqual(call.args, (["ldd", root / relative],
                                            {**environment, "LD_LIBRARY_PATH": str(root / "usr/lib")}, root))
            self.assertNotIn("LD_LIBRARY_PATH", environment)

    def test_portable_dependencies_report_missing_libraries_in_all_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            relatives = self.dependency_fixture(root)
            with patch(__name__ + ".run", return_value="libmissing.so => not found\n") as command:
                with self.assertRaises(SmokeError) as failure:
                    validate_portable_dependencies(root, {}, root)
            self.assertEqual(command.call_count, len(relatives))
            for relative in relatives:
                self.assertIn(relative, str(failure.exception))

    def test_portable_dependencies_reject_non_elf_applications_and_plugins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.dependency_fixture(root)
            for relative in ("usr/bin/connectcoin-qt", "usr/plugins/platforms/libqxcb.so"):
                with self.subTest(relative=relative):
                    target = root / relative
                    target.write_bytes(b"#!/bin/sh\nexit 0\n")
                    with patch(__name__ + ".run", return_value="libc.so.6 => /lib/libc.so.6"):
                        with self.assertRaisesRegex(SmokeError, "Expected an ELF"):
                            validate_portable_dependencies(root, {}, root)
                    target.write_bytes(b"\x7fELFfixture")

    def test_portable_dependencies_require_native_apps_and_platform_plugins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.dependency_fixture(root)
            for relative in ("usr/bin/connectcoin-cli", "usr/plugins/platforms/libqxcb.so",
                             "usr/plugins/platforms/libqminimal.so"):
                with self.subTest(relative=relative):
                    target = root / relative
                    target.unlink()
                    with patch(__name__ + ".run", return_value="libc.so.6 => /lib/libc.so.6"):
                        with self.assertRaises((SmokeError, OSError)):
                            validate_portable_dependencies(root, {}, root)
                    target.write_bytes(b"\x7fELFfixture")

    def test_portable_dependencies_reject_ldd_failure_and_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.dependency_fixture(root)
            for result in (subprocess.CompletedProcess([], 1, "", "not a dynamic executable"),
                           subprocess.TimeoutExpired("ldd", RPC_TIMEOUT)):
                with self.subTest(result=result):
                    options = {"side_effect": result} if isinstance(result, Exception) else {"return_value": result}
                    with patch.object(subprocess, "run", **options):
                        with self.assertRaises(SmokeError):
                            validate_portable_dependencies(root, {}, root)

    def test_portable_dependencies_include_additional_plugins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.dependency_fixture(root)
            plugin = root / "usr/plugins/imageformats/libqjpeg.so"
            plugin.parent.mkdir()
            plugin.write_bytes(b"\x7fELFfixture")
            with patch(__name__ + ".run", return_value="libc.so.6 => /lib/libc.so.6"):
                checked = validate_portable_dependencies(root, {}, root)
            self.assertIn("usr/plugins/imageformats/libqjpeg.so", checked)

    def test_version_validation(self):
        for title in ("", "daemon ", "RPC client "):
            self.assertEqual(version_from_output(f"ConnectCoin Core {title}version v1.0.0\nCopyright"), "v1.0.0")
        for text in ("Other Core version v1.0.0", "ConnectCoin Core version v1.0.0.1", "", "v1.0.0"):
            with self.assertRaises(SmokeError):
                version_from_output(text)

    def test_isolation_replaces_inherited_profile_and_loader_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.dict(os.environ, {"HOME": "/real-profile", "XDG_CONFIG_HOME": "/real-config",
                                         "LD_LIBRARY_PATH": "/developer-libs", "QT_PLUGIN_PATH": "/developer-qt"}):
                environment = isolated_environment(root)
            for key in ("HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
                self.assertTrue(Path(environment[key]).is_relative_to(root))
            self.assertNotIn("LD_LIBRARY_PATH", environment)
            self.assertNotIn("QT_PLUGIN_PATH", environment)
            self.assertEqual(environment["QT_QPA_PLATFORM"], "minimal")

    def test_rejects_paths_outside_the_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "install"
            prefix.mkdir()
            (root / "outside").touch()
            with self.assertRaises(SmokeError):
                installed_file(prefix, "../outside")

    def test_daemon_uses_explicit_isolation_and_disables_networking(self):
        arguments = daemon_arguments(Path("connectcoind"), Path("temporary-data"), Path("unique-cookie"), 12345)
        for argument in ("-regtest", "-datadir=temporary-data", "-rpccookiefile=unique-cookie",
                         "-noconf", "-nosettings", "-daemon=0", "-networkactive=0", "-listen=0",
                         "-dnsseed=0", "-fixedseeds=0", "-noconnect", "-randomxfast=0",
                         "-rpcbind=127.0.0.1", "-rpcport=12345"):
            self.assertIn(argument, arguments)

    def test_portable_mode_uses_root_wrappers_and_usr_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for target in TARGETS:
                (root / target).write_text("launcher", encoding="utf-8")
            desktop = root / "usr" / DESKTOP_FILE
            desktop.parent.mkdir(parents=True)
            desktop.write_text("[Desktop Entry]\nType=Application\nIcon=connectcoin\nExec=connectcoin-qt\n", encoding="utf-8")
            for name in (ICON_FILE, FALLBACK_ICON):
                icon = root / "usr" / name
                icon.parent.mkdir(parents=True, exist_ok=True)
                icon.write_bytes(b"\x89PNG\r\n\x1a\n")
            manuals = root / "usr/share/man/man1"
            manuals.mkdir(parents=True)
            for target in TARGETS:
                (manuals / f"{target}.1").write_text(f'.TH "{target.upper()}" "1"\n.SH NAME\n{target} - ConnectCoin Core\n', encoding="utf-8")
            with patch.object(os, "access", return_value=True):
                binaries = validate_layout(root, portable=True)
            self.assertEqual(binaries, {target: root / target for target in TARGETS})
            (manuals / "connectcoin-qt.1").unlink()
            with patch.object(os, "access", return_value=True):
                with self.assertRaisesRegex(SmokeError, "Missing manual page:.*connectcoin-qt"):
                    validate_layout(root, portable=True)
            with self.assertRaises(FileNotFoundError):
                validate_layout(root)

    def test_manual_validation_accepts_real_gzip_and_case_insensitive_titles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compressed = root / "connectcoin-qt.1.gz"
            compressed.write_bytes(gzip.compress(b'.TH "ConnectCoin\\-Qt" "1"\n.SH NAME\nconnectcoin-qt - graphical wallet\n'))
            validate_manual(root, "connectcoin-qt.1", "connectcoin-qt")
            (root / "connectcoind.1").write_text('.TH CONNECTCOIND "1"\n.SH NAME\nconnectcoind - full node\n', encoding="utf-8")
            validate_manual(root, "connectcoind.1", "connectcoind")
            with self.assertRaisesRegex(SmokeError, "Missing manual"):
                validate_manual(root, "connectcoin-cli.1", "connectcoin-cli")

    def test_manual_validation_rejects_placeholders_and_incorrect_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for suffix in ("", ".gz"):
                for content in (
                    '.TH CONNECTCOIN "1"\nThis is a PlaceHolder file.\n',
                    '.TH CONNECTCOIND "1"\nconnectcoin\n',
                    'connectcoin - missing roff header\n',
                ):
                    with self.subTest(suffix=suffix, content=content):
                        data = content.encode("utf-8")
                        (root / ("connectcoin.1" + suffix)).write_bytes(gzip.compress(data) if suffix else data)
                        with self.assertRaises(SmokeError):
                            validate_manual(root, "connectcoin.1", "connectcoin")
                (root / ("connectcoin.1" + suffix)).unlink()

    def test_manual_validation_bounds_decompression_and_rejects_truncated_gzip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manual = root / "connectcoin.1.gz"
            manual.write_bytes(gzip.compress(b'x' * (MAX_MANUAL_BYTES + 1)))
            with self.assertRaisesRegex(SmokeError, "exceeds 1 MiB"):
                validate_manual(root, "connectcoin.1", "connectcoin")
            manual.write_bytes(gzip.compress(b'.TH CONNECTCOIN "1"\n')[:-4])
            with self.assertRaisesRegex(SmokeError, "Unreadable manual"):
                validate_manual(root, "connectcoin.1", "connectcoin")

    def test_validated_launcher_keeps_its_invocation_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            resolved = root / "AppRun"
            resolved.touch()
            with patch.object(Path, "resolve", return_value=resolved):
                self.assertEqual(installed_file(root, "connectcoin-cli"), root / "connectcoin-cli")

    def test_cleanup_only_signals_the_owned_child_when_rpc_stop_fails(self):
        process = Mock()
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("owned-daemon", 10), 0]
        rpc = Mock(side_effect=SmokeError("RPC unavailable"))
        stop_owned_process(process, rpc)
        rpc.assert_called_once_with("stop", timeout=5)
        process.terminate.assert_called_once_with()
        process.kill.assert_called_once_with()
        self.assertEqual(process.wait.call_count, 2)
        exited = Mock()
        exited.poll.return_value = 0
        stop_owned_process(exited, rpc)
        exited.terminate.assert_not_called()
        exited.kill.assert_not_called()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, help="Installation root containing bin/ and share/, or an AppDir with --portable")
    parser.add_argument("--portable", action="store_true", help="Run AppDir root wrappers; desktop assets are under usr/")
    parser.add_argument("--output", type=Path, help="Optional JSON result metadata")
    parser.add_argument("--self-test", action="store_true", help="Run portable verifier regression tests")
    options = parser.parse_args()
    if options.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(RegressionTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if not sys.platform.startswith("linux") or options.prefix is None:
        parser.error("Run on Linux and specify --prefix (or use --self-test).")
    prefix = options.prefix.resolve(strict=True)
    binaries = validate_layout(prefix, portable=options.portable)
    with tempfile.TemporaryDirectory(prefix="connectcoin-linux-smoke-") as temporary:
        root = Path(temporary)
        environment = isolated_environment(root)
        dependencies = validate_portable_dependencies(prefix, environment, root) if options.portable else []
        commands = smoke_help(binaries, environment, root)
        regtest = smoke_regtest(binaries, environment, root)
    data_prefix = "usr/" if options.portable else ""
    report = {"prefix": str(prefix), "portable": options.portable, "commands": commands, "regtest": regtest,
              "desktop_file": data_prefix + DESKTOP_FILE,
              "icons": [data_prefix + ICON_FILE, data_prefix + FALLBACK_ICON],
              "manuals_checked": list(TARGETS),
              "runtime_dependencies_checked": dependencies,
              "temporary_profile_removed": True,
              "gui_coverage": "Help/version under the minimal Qt platform; no interactive GUI launch."}
    if options.output is not None:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(commands)} help/version commands, {len(TARGETS)} manual pages, desktop icons, isolated regtest wallet/address, graceful shutdown")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (SmokeError, OSError, ValueError, KeyError, configparser.Error) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
