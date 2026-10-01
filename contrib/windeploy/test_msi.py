#!/usr/bin/env python3
"""Verify a Core MSI without installing it or opening the wallet GUI.

Requires Windows, Python 3 and GNU objdump. Administrative extraction and all
diagnostics remain in a new build/msi-test-* directory for inspection. This is
not an install/upgrade/uninstall test; run those on a disposable clean Windows VM.
Run pure validation regression tests on any platform with --self-test.
"""
# Copyright (c) 2026 The ConnectCoin Core developers. MIT license.
import argparse
import base64
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
TARGETS = {"connectcoin", "connectcoin-qt", "connectcoind", "connectcoin-cli",
           "connectcoin-tx", "connectcoin-wallet", "connectcoin-util"}
UPGRADE_CODE = "{68B91A0E-90D2-4B52-B9F5-5DE859A401DB}"
# Fail closed before administrative extraction. Standard MSI actions can write
# registry values, move files, or change PATH without any CustomAction rows.
# These are the tables emitted by core.wxs/WixUI_InstallDir, plus signing tables.
ALLOWED_TABLES = set("""
_Validation AdminExecuteSequence AdminUISequence AdvtExecuteSequence AppSearch
Binary CheckBox Component Control ControlCondition ControlEvent CustomAction
Dialog Directory EventMapping Feature FeatureComponents File Icon
InstallExecuteSequence InstallUISequence LaunchCondition ListBox Media
MsiDigitalCertificate MsiDigitalSignature MsiFileHash MsiShortcutProperty
Property RadioButton Registry RegLocator RemoveFile Shortcut Signature
TextStyle UIText Upgrade
""".split())
ADMIN_EXECUTE_ACTIONS = (
    "CostInitialize", "FileCost", "CostFinalize", "InstallValidate",
    "InstallInitialize", "InstallAdminPackage", "InstallFiles", "InstallFinalize",
)
ADMIN_UI_ACTIONS = ("CostInitialize", "FileCost", "CostFinalize", "ExecuteAction")
INSTALL_EXECUTE_ACTIONS = (
    "FindRelatedProducts", "AppSearch", "LaunchConditions", "ValidateProductID",
    "CostInitialize", "FileCost", "CostFinalize", "MigrateFeatureStates",
    "InstallValidate", "InstallInitialize", "RemoveExistingProducts",
    "ProcessComponents", "UnpublishFeatures", "RemoveRegistryValues",
    "RemoveShortcuts", "RemoveFiles", "RemoveFolders", "CreateFolders",
    "InstallFiles", "CreateShortcuts", "WriteRegistryValues", "RegisterUser",
    "RegisterProduct", "PublishFeatures", "PublishProduct", "InstallFinalize",
)
REGISTRY_VALUES = {
    ("Software\\ConnectCoin Core\\Installer", "InstallDir", "[INSTALLFOLDER]", "InstallerRegistration"),
    ("Software\\Classes\\ConnectCoinCore.PaymentLink", "", "URL:ConnectCoin payment", "PaymentLinkRegistration"),
    ("Software\\Classes\\ConnectCoinCore.PaymentLink", "URL Protocol", "", "PaymentLinkRegistration"),
    ("Software\\Classes\\ConnectCoinCore.PaymentLink", "AppUserModelID", "ConnectCoin.Core", "PaymentLinkRegistration"),
    ("Software\\Classes\\ConnectCoinCore.PaymentLink\\DefaultIcon", "", '"[#GuiExe]",0', "PaymentLinkRegistration"),
    ("Software\\Classes\\ConnectCoinCore.PaymentLink\\shell\\open\\command", "", '"[#GuiExe]" "%1"', "PaymentLinkRegistration"),
    ("Software\\ConnectCoin Core\\Capabilities", "ApplicationName", "ConnectCoin Core", "PaymentLinkRegistration"),
    ("Software\\ConnectCoin Core\\Capabilities", "ApplicationDescription", "ConnectCoin wallet and full node", "PaymentLinkRegistration"),
    ("Software\\ConnectCoin Core\\Capabilities", "ApplicationIcon", '"[#GuiExe]",0', "PaymentLinkRegistration"),
    ("Software\\ConnectCoin Core\\Capabilities\\URLAssociations", "connectcoin", "ConnectCoinCore.PaymentLink", "PaymentLinkRegistration"),
    ("Software\\RegisteredApplications", "ConnectCoin Core", "Software\\ConnectCoin Core\\Capabilities", "PaymentLinkRegistration"),
}
# Deliberately do not treat every DLL in System32 or PATH as a Windows runtime.
# In particular VC redistributables and Qt must be present in the payload.
# Windows SDK ICU (icuuc/icuin) is built into Windows 10 1703 and later:
# https://learn.microsoft.com/windows/win32/intl/international-components-for-unicode--icu-
WINDOWS_DLLS = set("""
advapi32 authz avrt bcrypt bcryptprimitives cfgmgr32 combase comctl32 comdlg32
crypt32 cryptbase cryptsp d2d1 d3d9 d3d11 d3d12 dbghelp dhcpcsvc dnsapi dsound
dwmapi dwrite dxcore dxgi gdi32 glu32 icuin icuuc imagehlp imm32 iphlpapi kernel32 kernelbase
mpr msimg32 msvcrt ncrypt netapi32 normaliz ntdll ole32 oleaut32 opengl32 powrprof
propsys psapi rpcrt4 secur32 setupapi shcore shell32 shlwapi synchronization
ucrtbase uiautomationcore user32 userenv usp10 uxtheme version winhttp wininet winmm winscard
winspool wintrust wldap32 ws2_32 wtsapi32
""".split())


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_manifest(manifest):
    files = manifest["files"]
    require(isinstance(files, list) and files, "Manifest has no files")
    indexed = {}
    for entry in files:
        path = entry["path"]
        parsed = PurePosixPath(path)
        require(not parsed.is_absolute() and ".." not in parsed.parts and
                "\\" not in path and ":" not in path and parsed.as_posix() == path,
                f"Unsafe manifest path: {path}")
        key = path.lower()
        require(key not in indexed, f"Duplicate manifest path: {path}")
        require(re.fullmatch(r"[a-f0-9]{64}", entry["sha256"]) is not None,
                f"Invalid SHA-256: {path}")
        require(isinstance(entry["size"], int) and entry["size"] >= 0,
                f"Invalid file size: {path}")
        parts = set(part.lower() for part in parsed.parts)
        require(not parts.intersection({"test", "tests", "fuzz", "wallets", "blocks",
                                        "chainstate", "backups", "regtest", "testnet3",
                                        "testnet4", "signet"}),
                f"Test or user-data directory in payload: {path}")
        require(parsed.suffix.lower() not in {".pdb", ".obj", ".lib", ".ilk"},
                f"Development artifact in payload: {path}")
        require(parsed.name.lower() not in {"wallet.dat", "settings.json", "debug.log",
                                            "peers.dat", "banlist.json", "mempool.dat"},
                f"User data in payload: {path}")
        require(parsed.name.lower() != "connectcoin.conf" or
                path == "share/examples/connectcoin.conf", f"Active user config in payload: {path}")
        require(not parsed.name.lower().startswith("qt6test"), f"Qt test library in payload: {path}")
        if parsed.suffix.lower() == ".exe":
            require(path in {f"bin/{target}.exe" for target in TARGETS},
                    f"Unexpected executable: {path}")
        indexed[key] = entry
    expected = {f"bin/{target}.exe" for target in TARGETS}
    expected.update({"bin/qt6core.dll", "bin/qt6gui.dll", "bin/qt6widgets.dll",
                     "bin/platforms/qwindows.dll", "bin/styles/qmodernwindowsstyle.dll",
                     "bin/msvcp140.dll", "bin/msvcp140_1.dll", "bin/msvcp140_2.dll",
                     "bin/msvcp140_atomic_wait.dll", "bin/vcruntime140.dll",
                     "bin/vcruntime140_1.dll", "copying.txt", "readme.txt", "build-info.json",
                     "share/examples/connectcoin.conf", "share/rpcauth/rpcauth.py"})
    require(expected <= indexed.keys(), f"Required files missing: {sorted(expected - indexed.keys())}")
    return indexed


def powershell(script, environment):
    encoded = base64.b64encode(script.encode("utf-16le")).decode("ascii")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                             "-EncodedCommand", encoded], env=environment,
                            capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return json.loads(result.stdout.decode("utf-8-sig"))


def inspect_database(msi):
    environment = dict(os.environ, CONNECTCOIN_TEST_MSI=str(msi))
    # OpenDatabase mode 0 is read-only. No package actions execute here.
    return powershell(r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase($env:CONNECTCOIN_TEST_MSI, 0)
$tables = @{}
$view = $db.OpenView('SELECT `Name` FROM `_Tables`')
$view.Execute()
while ($record = $view.Fetch()) { $tables[$record.StringData(1)] = $true }
$view.Close()
$result = [ordered]@{}
foreach ($name in @('Property', 'Control', 'Directory', 'Component', 'File', 'Registry',
    'Shortcut', 'MsiShortcutProperty', 'Upgrade', 'LaunchCondition', 'CustomAction', 'RemoveFile',
    'InstallExecuteSequence', 'AdminExecuteSequence', 'AdminUISequence',
    'ServiceInstall', 'ServiceControl', 'WixFirewallException', 'Wix4FirewallException')) {
    $rows = @()
    if ($tables.ContainsKey($name)) {
        $view = $db.OpenView(('SELECT * FROM `{0}`' -f $name))
        $view.Execute()
        $columns = $view.ColumnInfo(0)
        while ($record = $view.Fetch()) {
            $row = [ordered]@{}
            for ($i = 1; $i -le $record.FieldCount(); $i++) {
                $row[$columns.StringData($i)] = $record.StringData($i)
            }
            $rows += $row
        }
        $view.Close()
    }
    $result[$name] = @($rows)
}
$icons = @()
if ($tables.ContainsKey('Icon')) {
    $view = $db.OpenView('SELECT `Name` FROM `Icon`')
    $view.Execute()
    while ($record = $view.Fetch()) { $icons += $record.StringData(1) }
    $view.Close()
}
$result['IconNames'] = $icons
$result['TableNames'] = @($tables.Keys)
$summary = $db.SummaryInformation(0)
$result['SummaryTemplate'] = $summary.Property(7)
$result | ConvertTo-Json -Depth 8 -Compress
''', environment)


def validate_installer_language(properties, summary_template):
    require(properties.get("ProductLanguage") == "1033", "Installer language must be English (en-US)")
    require(summary_template == "x64;1033", "Installer summary must identify x64 English (en-US)")


def validate_english_wizard(controls):
    buttons = [row["Text"] for row in controls if row["Type"] == "PushButton"]
    for label in ("Next", "Back", "Cancel", "Install", "Finish"):
        require(any(re.search(rf"\b{label}\b", text) for text in buttons),
                f"English wizard button missing: {label}")


def validate_sequences(tables):
    for name, required in (("AdminExecuteSequence", ADMIN_EXECUTE_ACTIONS),
                           ("AdminUISequence", ADMIN_UI_ACTIONS),
                           ("InstallExecuteSequence", INSTALL_EXECUTE_ACTIONS)):
        rows = {row["Action"]: row for row in tables[name]}
        require(len(rows) == len(tables[name]), f"Duplicate action in {name}")
        dialogs = {"ExitDialog": -1, "UserExit": -2, "FatalError": -3} if name == "AdminUISequence" else {}
        require(set(rows) == set(required) | dialogs.keys(), f"Unexpected or missing actions in {name}")
        require(all(not row["Condition"] for row in rows.values()),
                f"Required actions must be unconditional in {name}")
        sequence = [int(rows[action]["Sequence"]) for action in required]
        require(sequence[0] > 0 and all(left < right for left, right in zip(sequence, sequence[1:])),
                f"Incorrect action order in {name}")
        require(all(int(rows[action]["Sequence"]) == position for action, position in dialogs.items()),
                f"Incorrect exit dialog sequence in {name}")


def validate_schema(tables):
    unexpected = set(tables["TableNames"]) - ALLOWED_TABLES
    require(not unexpected, f"Unexpected MSI tables (not approved for extraction): {sorted(unexpected)}")
    validate_sequences(tables)


def validate_registry(registry):
    require(all(row["Root"] == "2" for row in registry), "Unexpected registry root")
    actual = {(row["Key"], row["Name"], row["Value"], row["Component_"]) for row in registry}
    require(actual == REGISTRY_VALUES and len(registry) == len(REGISTRY_VALUES),
            "Registry writes differ from the exact Core registration")


def validate_shortcut_icons(tables, properties):
    require(tables.get("IconNames") == ["CoreIcon.exe"],
            "Shortcut icon must have an .exe identifier matching the GUI")
    require(properties.get("ARPPRODUCTICON") == "CoreIcon.exe", "Installed-app icon missing")
    for shortcut in tables["Shortcut"]:
        require(shortcut["Icon_"] == "CoreIcon.exe" and shortcut["IconIndex"] == "0",
                f"Invalid shortcut icon: {shortcut['Shortcut']}")
    expected = {(row["Shortcut"], "System.AppUserModel.ID", "ConnectCoin.Core")
                for row in tables["Shortcut"]}
    actual = {(row["Shortcut_"], row["PropertyKey"], row["PropVariantValue"])
              for row in tables.get("MsiShortcutProperty", [])}
    require(actual == expected, "Shortcut AppUserModelID must match the GUI")


def validate_upgrade_policy(tables, version):
    # Keep all three ranges language-independent: older packages used pt-BR.
    # Check the complete rows, including removal scope and failure behavior.
    expected = {
        (UPGRADE_CODE, "", version, "", "1", "", "WIX_UPGRADE_DETECTED"),
        (UPGRADE_CODE, version, "", "", "2", "", "WIX_DOWNGRADE_DETECTED"),
        (UPGRADE_CODE, version, version, "", "770", "", "SAMEVERSIONFOUND"),
    }
    actual = {(row["UpgradeCode"].upper(), row["VersionMin"], row["VersionMax"],
               row["Language"], row["Attributes"], row["Remove"], row["ActionProperty"])
              for row in tables["Upgrade"]}
    require(actual == expected and len(tables["Upgrade"]) == len(expected),
            "Upgrade policy must detect all languages and replace only older complete products")
    conditions = {
        "NOT WIX_DOWNGRADE_DETECTED",
        "Installed OR NOT SAMEVERSIONFOUND",
        "Installed OR (VersionNT64 AND WINDOWSBUILDNUMBER >= 17763)",
    }
    require({row["Condition"] for row in tables["LaunchCondition"]} == conditions and
            len(tables["LaunchCondition"]) == len(conditions),
            "Launch conditions must exactly enforce version and supported OS requirements")


def extract_icon_stream(msi_path, destination):
    """Read just the Icon stream through MSI APIs; never execute MSI actions."""
    msi = ctypes.WinDLL("msi", use_last_error=True)
    handle = wintypes.UINT
    pointer = ctypes.POINTER(handle)
    signatures = {
        "MsiOpenDatabaseW": ([wintypes.LPCWSTR, wintypes.LPCWSTR, pointer], wintypes.UINT),
        "MsiDatabaseOpenViewW": ([handle, wintypes.LPCWSTR, pointer], wintypes.UINT),
        "MsiViewExecute": ([handle, handle], wintypes.UINT),
        "MsiViewFetch": ([handle, pointer], wintypes.UINT),
        "MsiRecordReadStream": ([handle, wintypes.UINT, ctypes.c_void_p,
                                ctypes.POINTER(wintypes.DWORD)], wintypes.UINT),
        "MsiCloseHandle": ([handle], wintypes.UINT),
    }
    for name, (arguments, result) in signatures.items():
        getattr(msi, name).argtypes = arguments
        getattr(msi, name).restype = result
    database, view, record = handle(), handle(), handle()

    def check(result):
        require(result == 0, f"MSI icon stream read failed: {result}")

    try:
        check(msi.MsiOpenDatabaseW(str(msi_path), None, ctypes.byref(database)))
        check(msi.MsiDatabaseOpenViewW(database,
              "SELECT `Data` FROM `Icon` WHERE `Name` = 'CoreIcon.exe'", ctypes.byref(view)))
        check(msi.MsiViewExecute(view, 0))
        check(msi.MsiViewFetch(view, ctypes.byref(record)))
        data = bytearray()
        buffer = ctypes.create_string_buffer(65536)
        while True:
            length = wintypes.DWORD(len(buffer))
            check(msi.MsiRecordReadStream(record, 1, buffer, ctypes.byref(length)))
            if length.value == 0:
                break
            data.extend(buffer.raw[:length.value])
            require(len(data) <= 1024 * 1024, "Shortcut icon PE must stay small (at most 1 MiB)")
        require(data[:2] == b"MZ", "Shortcut icon stream is not a PE resource")
        destination.write_bytes(data)
    finally:
        for value in (record, view, database):
            if value.value:
                msi.MsiCloseHandle(value)


def inspect_shell_icons(path):
    """Use the Windows Shell's real extractor at common taskbar/DPI sizes."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    extract = user32.PrivateExtractIconsW
    extract.argtypes = [wintypes.LPCWSTR, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                        ctypes.POINTER(wintypes.HICON), ctypes.POINTER(wintypes.UINT),
                        wintypes.UINT, wintypes.UINT]
    extract.restype = wintypes.UINT
    user32.DestroyIcon.argtypes = [wintypes.HICON]
    user32.DestroyIcon.restype = wintypes.BOOL
    for size in (16, 24, 32, 48, 64, 128, 256):
        icon, identifier = wintypes.HICON(), wintypes.UINT()
        count = extract(str(path), 0, size, size, ctypes.byref(icon), ctypes.byref(identifier), 1, 0)
        try:
            require(count == 1 and icon.value, f"Cannot extract {size}px icon at index 0: {path}")
        finally:
            if icon.value:
                user32.DestroyIcon(icon)
    return {"path": str(path), "index": 0, "sizes": [16, 24, 32, 48, 64, 128, 256]}


def validate_database(tables, manifest):
    validate_schema(tables)
    properties = {row["Property"]: row["Value"] for row in tables["Property"]}
    validate_installer_language(properties, tables["SummaryTemplate"])
    validate_english_wizard(tables["Control"])
    validate_shortcut_icons(tables, properties)
    require(properties.get("ProductName") == "ConnectCoin Core", "Wrong product name")
    require(properties.get("ProductVersion") == manifest["build"]["msi_version"], "Wrong MSI version")
    require(properties.get("UpgradeCode", "").upper() == UPGRADE_CODE, "Upgrade identity changed")
    require(properties.get("ALLUSERS") == "1", "Expected per-machine installation")
    require(properties.get("MSIRESTARTMANAGERCONTROL") == "Disable", "Restart Manager must be disabled")
    require(properties.get("REBOOT") == "ReallySuppress", "Installer may reboot unexpectedly")
    require(tables["SummaryTemplate"].startswith("x64;"), "MSI is not x64")
    directories = {row["Directory"]: row for row in tables["Directory"]}
    require(directories["INSTALLFOLDER"]["Directory_Parent"] == "ProgramFiles64Folder",
            "Install directory is not Program Files (x64)")
    require(not {"AppDataFolder", "LocalAppDataFolder", "PersonalFolder"}.intersection(directories),
            "Installer contains user-data directories")
    components = {row["Component"]: row for row in tables["Component"]}
    require(all(int(row["Attributes"]) & 256 for row in components.values()), "Non-x64 component")

    def relative_directory(directory, visited=None):
        if directory == "INSTALLFOLDER":
            return PurePosixPath()
        visited = set() if visited is None else visited
        require(directory not in visited, "Directory cycle in MSI")
        visited.add(directory)
        row = directories[directory]
        name = row["DefaultDir"].split(":")[0].split("|")[-1]
        return relative_directory(row["Directory_Parent"], visited) / name

    actual = {}
    for row in tables["File"]:
        component = components[row["Component_"]]
        path = (relative_directory(component["Directory_"]) /
                row["FileName"].split("|")[-1]).as_posix().lower()
        require(path not in actual, f"Duplicate File table path: {path}")
        actual[path] = row
    expected = {entry["path"].lower(): entry for entry in manifest["files"]}
    require(actual.keys() == expected.keys(), "MSI File table differs from manifest")
    require(all(int(actual[path]["FileSize"]) == entry["size"] for path, entry in expected.items()),
            "MSI File table sizes differ from manifest")
    gui = actual["bin/connectcoin-qt.exe"]
    shortcuts = {row["Shortcut"]: row for row in tables["Shortcut"]}
    require(shortcuts.keys() == {"StartMenuShortcut", "DesktopShortcut"}, "Unexpected shortcuts")
    for key, directory in (("StartMenuShortcut", "CoreMenuFolder"), ("DesktopShortcut", "DesktopFolder")):
        shortcut = shortcuts[key]
        require(shortcut["Directory_"] == directory and shortcut["Component_"] == gui["Component_"]
                and shortcut["Target"] == "Core" and shortcut["WkDir"] == "BINFOLDER"
                and not shortcut["Arguments"], f"Invalid GUI shortcut: {key}")
    validate_registry(tables["Registry"])

    validate_upgrade_policy(tables, properties["ProductVersion"])
    actions = tables["CustomAction"]
    for row in actions:
        require(int(row["Type"]) in {1, 65} and
                row["Source"] == "WixUiCa_X64" and
                row["Target"] in {"PrintEula", "ValidatePath"},
                f"Unexpected custom action: {row}")
    custom_names = {row["Action"] for row in actions}
    require(not any(row["Action"] in custom_names for name in ("AdminExecuteSequence", "AdminUISequence")
                    for row in tables[name]), "Administrative extraction invokes custom code")
    for row in tables["RemoveFile"]:
        require(not row["FileName"] and row["DirProperty"] == "CoreMenuFolder" and row["InstallMode"] == "2",
                f"Unexpected file deletion: {row}")
    return {"product_code": properties["ProductCode"], "upgrade_code": properties["UpgradeCode"],
            "language": properties["ProductLanguage"],
            "file_count": len(actual), "shortcuts": sorted(shortcuts)}


def compare_extraction(extracted, manifest):
    candidates = list(extracted.rglob("build-info.json"))
    require(len(candidates) == 1, "Could not identify exactly one extracted payload")
    payload = candidates[0].parent
    actual = {path.relative_to(payload).as_posix().lower(): path
              for path in payload.rglob("*") if path.is_file()}
    expected = {entry["path"].lower(): entry for entry in manifest["files"]}
    require(actual.keys() == expected.keys(), "Extracted payload differs from manifest file list")
    for name, entry in expected.items():
        path = actual[name]
        require(path.stat().st_size == entry["size"] and sha256(path) == entry["sha256"],
                f"Extracted payload hash/size mismatch: {name}")
    return payload


def dependency_available(name, payload_names):
    name = name.lower()
    return (name in payload_names or name.startswith(("api-ms-win-", "ext-ms-win-"))
            or name.removesuffix(".dll") in WINDOWS_DLLS)


def inspect_pe(payload, objdump):
    binaries = sorted(path for path in (payload / "bin").rglob("*")
                      if path.suffix.lower() in {".exe", ".dll"})
    # All ordinary imports must resolve alongside the executable. Qt plugin
    # directories do not satisfy a missing DLL in bin.
    local_names = {path.name.lower() for path in (payload / "bin").glob("*") if path.is_file()}
    result = {}
    for binary in binaries:
        output = subprocess.check_output([str(objdump), "-p", str(binary)],
                                         text=True, errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
        require("file format pei-x86-64" in output, f"Non-x64 PE file: {binary.name}")
        imports = sorted(set(re.findall(r"DLL Name:\s*(\S+)", output)))
        require(imports, f"Could not read imports: {binary.name}")
        missing = [name for name in imports if not dependency_available(name, local_names)]
        require(not missing, f"Missing non-Windows imports for {binary.name}: {missing}")
        result[binary.relative_to(payload).as_posix()] = imports
    return result


def validate_cli_version(text, core_version, executable):
    first_line = text.strip().splitlines()[0] if text.strip() else ""
    match = re.search(r"\bversion v(\S+)", first_line)
    observed = match[1] if match else ""
    require(first_line.startswith("ConnectCoin Core ") and
            re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", observed) is not None and
            observed.split("-", 1)[0] == core_version,
            f"{executable} version mismatch: expected {core_version}, got {observed!r}")
    return observed


def validate_gui_metadata(metadata, core_version):
    require("connectcoin" in (metadata["ProductName"] + metadata["FileDescription"]).lower(),
            "GUI resource metadata does not identify ConnectCoin")
    for field in ("FileVersion", "ProductVersion"):
        require(metadata.get(field) == core_version,
                f"GUI {field} mismatch: expected {core_version}, got {metadata.get(field)!r}")


def smoke_cli(payload, work, core_version):
    environment = dict(os.environ)
    profile = work / "smoke-profile"
    profile.mkdir()
    for key in ("APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TMP", "TEMP"):
        environment[key] = str(profile)
    # Do not accidentally satisfy dependencies from developer tool directories.
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    reports = []
    for target in sorted(TARGETS - {"connectcoin-qt"}):
        for option in ("--version", "--help"):
            command = [str(payload / "bin" / f"{target}.exe"), option]
            if target == "connectcoind":
                # The daemon initializes config before processing help/version.
                # Explicit isolation also bypasses Windows known-folder lookup.
                command += [f"-datadir={profile}", "-nosettings"]
            result = subprocess.run(command,
                                    cwd=profile, env=environment, capture_output=True,
                                    timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            text = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            (work / f"{target}-{option[2:]}.txt").write_text(text, encoding="utf-8")
            require(result.returncode == 0 and "connectcoin" in text.lower(),
                    f"Smoke failed: {target} {option} returned {result.returncode}: {text[:500]}")
            report = {"executable": target, "option": option, "exit_code": result.returncode}
            if option == "--version":
                report["version"] = validate_cli_version(text, core_version, target)
            reports.append(report)
    require(not list(profile.iterdir()), "Help/version unexpectedly wrote files in the isolated profile")
    return reports


class RegressionTests(unittest.TestCase):
    @staticmethod
    def sequence_tables():
        tables = {name: [{"Action": action, "Condition": "", "Sequence": str(index * 100)}
                         for index, action in enumerate(actions, 1)]
                  for name, actions in (("AdminExecuteSequence", ADMIN_EXECUTE_ACTIONS),
                                        ("AdminUISequence", ADMIN_UI_ACTIONS),
                                        ("InstallExecuteSequence", INSTALL_EXECUTE_ACTIONS))}
        tables["AdminUISequence"] += [
            {"Action": action, "Condition": "", "Sequence": str(sequence)}
            for action, sequence in (("ExitDialog", -1), ("UserExit", -2), ("FatalError", -3))]
        tables["TableNames"] = sorted(ALLOWED_TABLES)
        return tables

    def test_preflight_rejects_unapproved_tables(self):
        validate_schema(self.sequence_tables())
        for name in ("Environment", "MoveFile", "DuplicateFile", "IniFile", "RemoveIniFile",
                     "RemoveRegistry", "ServiceInstall", "ServiceControl", "SelfReg", "Class",
                     "Extension", "ProgId", "MIME", "ODBCDataSource", "Wix4FirewallException"):
            with self.subTest(table=name):
                tables = self.sequence_tables()
                tables["TableNames"].append(name)
                with self.assertRaisesRegex(ValueError, "Unexpected MSI tables"):
                    validate_schema(tables)

    def test_preflight_rejects_standard_admin_mutations(self):
        for table in ("AdminExecuteSequence", "AdminUISequence"):
            for action in ("WriteRegistryValues", "WriteEnvironmentStrings", "MoveFiles",
                           "RemoveFiles", "RegisterProduct", "RemoveExistingProducts", "CustomCode"):
                with self.subTest(table=table, action=action):
                    tables = self.sequence_tables()
                    tables[table].append({"Action": action, "Condition": "", "Sequence": "5000"})
                    with self.assertRaisesRegex(ValueError, "Unexpected or missing actions"):
                        validate_schema(tables)

    def test_required_actions_cannot_be_missing_conditional_or_reordered(self):
        for table in ("AdminExecuteSequence", "AdminUISequence", "InstallExecuteSequence"):
            for index, row in enumerate(self.sequence_tables()[table]):
                with self.subTest(table=table, action=row["Action"]):
                    tables = self.sequence_tables()
                    del tables[table][index]
                    with self.assertRaisesRegex(ValueError, "Unexpected or missing actions"):
                        validate_sequences(tables)
                    tables = self.sequence_tables()
                    tables[table][index]["Condition"] = "0"
                    with self.assertRaisesRegex(ValueError, "unconditional"):
                        validate_sequences(tables)
            tables = self.sequence_tables()
            tables[table][1]["Sequence"] = tables[table][0]["Sequence"]
            with self.assertRaisesRegex(ValueError, "action order"):
                validate_sequences(tables)
            tables = self.sequence_tables()
            tables[table].append(tables[table][0])
            with self.assertRaisesRegex(ValueError, "Duplicate action"):
                validate_sequences(tables)

    def test_registry_scope_is_exact(self):
        registry = [{"Root": "2", "Key": key, "Name": name, "Value": value, "Component_": component}
                    for key, name, value, component in sorted(REGISTRY_VALUES)]
        validate_registry(registry)
        for field, value in (("Root", "1"), ("Key", "Software\\Classes\\connectcoin"),
                             ("Name", "OtherWallet"), ("Value", "[BINFOLDER]"),
                             ("Component_", "OtherComponent")):
            with self.subTest(field=field):
                changed = [dict(row) for row in registry]
                changed[0][field] = value
                with self.assertRaisesRegex(ValueError, "registry root|exact Core registration"):
                    validate_registry(changed)
        for changed in (registry[:-1], registry + [registry[0]], registry + [{
                "Root": "2", "Key": "Software\\RegisteredApplications", "Name": "ConnectWallet",
                "Value": "Software\\ConnectCoin Core\\Capabilities", "Component_": "PaymentLinkRegistration"}]):
            with self.assertRaisesRegex(ValueError, "exact Core registration"):
                validate_registry(changed)

    @staticmethod
    def upgrade_tables():
        return {
            "Upgrade": [
                {"UpgradeCode": UPGRADE_CODE, "VersionMin": minimum, "VersionMax": maximum,
                 "Language": "", "Attributes": attributes, "Remove": "", "ActionProperty": action}
                for minimum, maximum, attributes, action in (
                    ("", "1.0.0", "1", "WIX_UPGRADE_DETECTED"),
                    ("1.0.0", "", "2", "WIX_DOWNGRADE_DETECTED"),
                    ("1.0.0", "1.0.0", "770", "SAMEVERSIONFOUND"))
            ],
            "LaunchCondition": [{"Condition": condition} for condition in (
                "NOT WIX_DOWNGRADE_DETECTED",
                "Installed OR NOT SAMEVERSIONFOUND",
                "Installed OR (VersionNT64 AND WINDOWSBUILDNUMBER >= 17763)",
            )],
        }

    def test_upgrade_policy_requires_exact_ranges_and_full_removal(self):
        validate_upgrade_policy(self.upgrade_tables(), "1.0.0")
        for index, field, value in (
            (0, "UpgradeCode", "{00000000-0000-0000-0000-000000000000}"),
            (0, "VersionMin", "0.9.0"),
            (0, "VersionMax", "2.0.0"),
            (0, "Attributes", "5"),  # IgnoreRemoveFailure must remain disabled.
            (0, "Attributes", "513"),  # Never remove another same-version package.
            (0, "Remove", "NonexistentFeature"),
            (0, "ActionProperty", "UNUSED_UPGRADE_PROPERTY"),
            (1, "VersionMin", "2.0.0"),
            (1, "VersionMax", "2.0.0"),
            (1, "Attributes", "0"),  # Downgrade detection must never remove a product.
            (2, "Attributes", "2"),  # Same-version bounds must both be inclusive.
        ):
            with self.subTest(index=index, field=field, value=value):
                tables = self.upgrade_tables()
                tables["Upgrade"][index][field] = value
                with self.assertRaisesRegex(ValueError, "Upgrade policy"):
                    validate_upgrade_policy(tables, "1.0.0")
        # Switching the wizard from Portuguese to English must neither bypass
        # a downgrade block nor leave an older Portuguese product installed.
        for index in range(3):
            for language in ("1033", "1046"):
                with self.subTest(index=index, language=language):
                    tables = self.upgrade_tables()
                    tables["Upgrade"][index]["Language"] = language
                    with self.assertRaisesRegex(ValueError, "Upgrade policy"):
                        validate_upgrade_policy(tables, "1.0.0")

    def test_launch_conditions_cannot_be_weakened_or_duplicated(self):
        for index in range(3):
            with self.subTest(index=index):
                tables = self.upgrade_tables()
                tables["LaunchCondition"][index]["Condition"] += " OR 1"
                with self.assertRaisesRegex(ValueError, "Launch conditions"):
                    validate_upgrade_policy(tables, "1.0.0")
        for name in ("Upgrade", "LaunchCondition"):
            for index in range(3):
                with self.subTest(table=name, index=index):
                    tables = self.upgrade_tables()
                    del tables[name][index]
                    with self.assertRaises(ValueError):
                        validate_upgrade_policy(tables, "1.0.0")
                    tables = self.upgrade_tables()
                    tables[name].append(tables[name][index])
                    with self.assertRaises(ValueError):
                        validate_upgrade_policy(tables, "1.0.0")

    def test_shortcut_icons_and_app_identity(self):
        tables = {"IconNames": ["CoreIcon.exe"], "Shortcut": [
            {"Shortcut": name, "Icon_": "CoreIcon.exe", "IconIndex": "0"}
            for name in ("StartMenuShortcut", "DesktopShortcut")],
            "MsiShortcutProperty": [
                {"Shortcut_": name, "PropertyKey": "System.AppUserModel.ID",
                 "PropVariantValue": "ConnectCoin.Core"}
                for name in ("StartMenuShortcut", "DesktopShortcut")]}
        properties = {"ARPPRODUCTICON": "CoreIcon.exe"}
        validate_shortcut_icons(tables, properties)
        for table, index, field, value in (
            ("Shortcut", 0, "Icon_", "CoreIcon"),
            ("Shortcut", 1, "IconIndex", ""),
            ("MsiShortcutProperty", 0, "PropVariantValue", "Other.App"),
            ("MsiShortcutProperty", 1, "Shortcut_", "MissingShortcut")):
            changed = json.loads(json.dumps(tables))
            changed[table][index][field] = value
            with self.subTest(table=table, field=field):
                with self.assertRaisesRegex(ValueError, "icon|AppUserModelID"):
                    validate_shortcut_icons(changed, properties)
        for names in (["CoreIcon"], ["CoreIcon.ico"], []):
            with self.assertRaisesRegex(ValueError, "identifier"):
                validate_shortcut_icons(dict(tables, IconNames=names), properties)
        with self.assertRaisesRegex(ValueError, "Installed-app icon"):
            validate_shortcut_icons(tables, {"ARPPRODUCTICON": "CoreIcon"})
        with self.assertRaisesRegex(ValueError, "AppUserModelID"):
            validate_shortcut_icons(dict(tables, MsiShortcutProperty=[]), properties)

    def test_wizard_buttons_must_be_english(self):
        controls = [{"Type": "PushButton", "Text": text}
                    for text in ("&Next", "&Back", "Cancel", "&Install", "&Finish")]
        validate_english_wizard(controls)
        controls[0]["Text"] = "Avancar"
        with self.assertRaisesRegex(ValueError, "English wizard button missing: Next"):
            validate_english_wizard(controls)

    def test_installer_language_must_be_english(self):
        validate_installer_language({"ProductLanguage": "1033"}, "x64;1033")
        for language, summary in (("1046", "x64;1046"), ("1033", "x64;1046"),
                                  ("1046", "x64;1033"), ("1033", "Intel;1033")):
            with self.subTest(language=language, summary=summary):
                with self.assertRaisesRegex(ValueError, "English"):
                    validate_installer_language({"ProductLanguage": language}, summary)

    def test_cli_versions_must_match_manifest(self):
        for version in ("1.0.0", "1.0.0-88360e0e5d05", "1.0.0-88360e0e5d05-dirty"):
            text = f"ConnectCoin Core daemon version v{version} connectcoind\n"
            self.assertEqual(validate_cli_version(text, "1.0.0", "connectcoind"), version)
        for text in ("ConnectCoin Core version v31.99.0-88360e0e5d05\n",
                     "ConnectCoin Core version v1.0.01\n",
                     "ConnectCoin Core version v1.0.0.1\n",
                     "ConnectCoin Core version v31.99.0\nExpected version v1.0.0\n",
                     "ConnectCoin Core version unknown\n", ""):
            with self.subTest(text=text):
                with self.assertRaisesRegex(ValueError, "version mismatch"):
                    validate_cli_version(text, "1.0.0", "connectcoin")

    def test_gui_versions_must_both_match_manifest(self):
        metadata = {"ProductName": "ConnectCoin Core", "FileDescription": "ConnectCoin Core GUI",
                    "FileVersion": "1.0.0", "ProductVersion": "1.0.0"}
        validate_gui_metadata(metadata, "1.0.0")
        for field in ("FileVersion", "ProductVersion"):
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, f"GUI {field} mismatch"):
                    validate_gui_metadata(dict(metadata, **{field: "31.99.0"}), "1.0.0")

    def test_dependency_rules_require_app_local_vc_and_qt(self):
        self.assertFalse(dependency_available("MSVCP140.dll", set()))
        self.assertFalse(dependency_available("VCRUNTIME140.dll", set()))
        self.assertFalse(dependency_available("Qt6Core.dll", set()))
        self.assertFalse(dependency_available("arbitrary-installed-tool.dll", set()))
        self.assertTrue(dependency_available("KERNEL32.dll", set()))
        self.assertTrue(dependency_available("api-ms-win-core-file-l1-1-0.dll", set()))
        self.assertTrue(dependency_available("MSVCP140.dll", {"msvcp140.dll"}))

    def test_rejects_unsafe_payload_before_required_file_check(self):
        for path in ("../wallet.dat", "bin/../wallet.dat", "C:/wallet.dat", "bin\\tool.exe",
                     "wallet.dat", "settings.json", "connectcoin.conf", "bin/Qt6Test.dll",
                     "bin/fuzz.exe", "bin/connectcoin-test.exe", "blocks/blk00000.dat", "bin/core.pdb"):
            with self.subTest(path=path):
                with self.assertRaisesRegex(ValueError, "Unsafe|User data|Active user config|Qt test|Unexpected executable|Test or user-data|Development"):
                    validate_manifest({"files": [{"path": path, "size": 0, "sha256": "0" * 64}]})

    def test_extraction_detects_hash_changes_and_extra_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            payload = directory / "ConnectCoin Core"
            payload.mkdir()
            file = payload / "build-info.json"
            file.write_text("{}", encoding="utf-8")
            manifest = {"files": [{"path": file.name, "size": file.stat().st_size, "sha256": sha256(file)}]}
            self.assertEqual(compare_extraction(directory, manifest), payload)
            file.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash/size mismatch"):
                compare_extraction(directory, manifest)
            file.write_text("{}", encoding="utf-8")
            (payload / "wallet.dat").touch()
            with self.assertRaisesRegex(ValueError, "file list"):
                compare_extraction(directory, manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("msi", nargs="?", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--objdump", type=Path, default=shutil.which("objdump"))
    parser.add_argument("--work-parent", type=Path, default=REPO / "build")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(RegressionTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if os.name != "nt" or options.msi is None or options.objdump is None:
        parser.error("Specify an MSI on Windows with objdump available (or --objdump PATH).")
    msi = options.msi.resolve(strict=True)
    manifest_path = options.manifest or msi.with_suffix(".manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate_manifest(manifest)
    core_version = manifest["build"]["core_version"]
    require(re.fullmatch(r"\d+\.\d+\.\d+", core_version) is not None, "Invalid manifest Core version")
    checksum_path = msi.with_suffix(msi.suffix + ".sha256")
    require(sha256(msi) == checksum_path.read_text(encoding="ascii").split()[0], "MSI checksum mismatch")
    tables = inspect_database(msi)
    database_report = validate_database(tables, manifest)
    options.work_parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="msi-test-", dir=options.work_parent.resolve()))
    print(f"Verification diagnostics: {work}", flush=True)
    (work / "database.json").write_text(json.dumps(tables, indent=2) + "\n", encoding="utf-8")
    extracted = work / "extracted"
    extracted.mkdir()
    # /a only creates an administrative source image. It does not register,
    # install, repair or uninstall the application on this machine.
    result = subprocess.run(["msiexec.exe", "/a", str(msi), "/qn", f"TARGETDIR={extracted}",
                             "REBOOT=ReallySuppress", "/l*v", str(work / "extract.log")],
                            creationflags=subprocess.CREATE_NO_WINDOW)
    require(result.returncode == 0, f"Administrative extraction failed: {result.returncode}; see extract.log")
    payload = compare_extraction(extracted, manifest)
    icon_resource = work / "CoreIcon.exe"
    extract_icon_stream(msi, icon_resource)
    icons = [inspect_shell_icons(path) for path in
             (icon_resource, payload / "bin/connectcoin-qt.exe")]
    dependencies = inspect_pe(payload, options.objdump)
    smoke = smoke_cli(payload, work, core_version)
    metadata = powershell(r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
(Get-Item -LiteralPath $env:CONNECTCOIN_TEST_GUI).VersionInfo |
    Select-Object FileVersion, ProductVersion, ProductName, FileDescription |
    ConvertTo-Json -Compress
''', dict(os.environ, CONNECTCOIN_TEST_GUI=str(payload / "bin/connectcoin-qt.exe")))
    validate_gui_metadata(metadata, core_version)
    report = {"msi": str(msi), "sha256": sha256(msi), "payload": str(payload),
              "core_version": core_version,
              "database": database_report, "pe_imports": dependencies, "cli_smoke": smoke,
              "gui_metadata": metadata, "shell_icons": icons, "limitations": [
                  "GUI launch is not tested: Windows help/version opens a modal dialog.",
                  "Install, repair, upgrade and uninstall require a disposable clean Windows VM."]}
    report_path = work / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(manifest['files'])} payload hashes, MSI policy, {len(dependencies)} PE files, "
          f"{len(smoke)} CLI smoke commands.\nReport: {report_path}", flush=True)
    print("GUI launch and installed-product lifecycle remain untested.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
