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
foreach ($name in @('Property', 'Directory', 'Component', 'File', 'Registry',
    'Shortcut', 'Upgrade', 'LaunchCondition', 'CustomAction', 'RemoveFile',
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
$result['TableNames'] = @($tables.Keys)
$summary = $db.SummaryInformation(0)
$result['SummaryTemplate'] = $summary.Property(7)
$result | ConvertTo-Json -Depth 8 -Compress
''', environment)


def validate_database(tables, manifest):
    properties = {row["Property"]: row["Value"] for row in tables["Property"]}
    require(properties.get("ProductName") == "ConnectCoin Core", "Wrong product name")
    require(properties.get("ProductVersion") == manifest["build"]["msi_version"], "Wrong MSI version")
    require(properties.get("UpgradeCode", "").upper() == UPGRADE_CODE, "Upgrade identity changed")
    require(properties.get("ALLUSERS") == "1", "Expected per-machine installation")
    require(properties.get("MSIRESTARTMANAGERCONTROL") == "Disable", "Restart Manager must be disabled")
    require(properties.get("REBOOT") == "ReallySuppress", "Installer may reboot unexpectedly")
    require(tables["SummaryTemplate"].startswith("x64;"), "MSI is not x64")
    for name in tables["TableNames"]:
        require("firewall" not in name.lower() and name not in {"ServiceInstall", "ServiceControl"},
                f"Unexpected system integration table: {name}")
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
    registry = tables["Registry"]
    require(all(row["Root"] == "2" for row in registry), "Unexpected registry root")
    require(all(row["Key"].startswith(("Software\\ConnectCoin Core\\", "Software\\Classes\\ConnectCoinCore.PaymentLink"))
                or row["Key"] == "Software\\RegisteredApplications" for row in registry),
            "Registry writes outside Core registration")
    commands = [row for row in registry if row["Key"].endswith("\\shell\\open\\command")]
    require(len(commands) == 1 and commands[0]["Value"] == '\"[#GuiExe]\" \"%1\"',
            "Payment URI command must quote executable and URI")
    require(any(row["Key"] == "Software\\ConnectCoin Core\\Capabilities\\URLAssociations"
                and row["Name"] == "connectcoin" and row["Value"] == "ConnectCoinCore.PaymentLink"
                for row in registry), "Missing payment handler capability")
    require(not any("userchoice" in row["Key"].lower() or
                    row["Key"].lower().startswith("software\\classes\\connectcoin\\")
                    for row in registry), "Installer overrides an existing protocol default")

    version = properties["ProductVersion"]
    upgrades = tables["Upgrade"]
    require(all(row["UpgradeCode"].upper() == UPGRADE_CODE for row in upgrades), "Unexpected Upgrade row")
    require(any(row["ActionProperty"] == "WIX_DOWNGRADE_DETECTED" and row["VersionMin"] == version
                and int(row["Attributes"]) & 2 for row in upgrades), "Missing downgrade detection")
    require(any(row["ActionProperty"] == "WIX_UPGRADE_DETECTED" and row["VersionMax"] == version
                and not int(row["Attributes"]) & (2 | 512) for row in upgrades),
            "Upgrade range must replace only older versions")
    require(any(row["ActionProperty"] == "SAMEVERSIONFOUND" and row["VersionMin"] == version
                and row["VersionMax"] == version and int(row["Attributes"]) & (2 | 256 | 512) == (2 | 256 | 512)
                for row in upgrades), "Missing same-version rebuild detection")
    conditions = {row["Condition"] for row in tables["LaunchCondition"]}
    require(any("NOT WIX_DOWNGRADE_DETECTED" in condition for condition in conditions), "Downgrade is not blocked")
    require("Installed OR NOT SAMEVERSIONFOUND" in conditions, "Same-version rebuild is not blocked")
    require(any("VersionNT64" in condition and "WINDOWSBUILDNUMBER >= 17763" in condition
                for condition in conditions), "Missing supported OS launch condition")
    sequence = {row["Action"]: int(row["Sequence"]) for row in tables["InstallExecuteSequence"]}
    require(sequence["InstallInitialize"] < sequence["RemoveExistingProducts"] < sequence["InstallFiles"],
            "Upgrade replacement must be inside the install transaction")
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
              "gui_metadata": metadata, "limitations": [
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
