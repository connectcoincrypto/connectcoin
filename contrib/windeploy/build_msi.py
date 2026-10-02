#!/usr/bin/env python3
"""Build an offline, x64 MSVC/vcpkg ConnectCoin Core MSI (WiX 5)."""
# Copyright (c) 2026 The ConnectCoin Core developers. MIT license.
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
TARGETS = ("connectcoin", "connectcoin-qt", "connectcoind", "connectcoin-cli",
           "connectcoin-tx", "connectcoin-wallet", "connectcoin-util")
HUGE_PAGES_HELPER = "connectcoin-huge-pages"
NS = "http://wixtoolset.org/schemas/v4/wxs"
ET.register_namespace("", NS)


def run(*args):
    print("+", subprocess.list2cmdline([str(a) for a in args]), flush=True)
    subprocess.run([str(a) for a in args], check=True)


def cache_value(cache, name):
    match = re.search(rf"^{re.escape(name)}:[^=]+=(.*)$", cache, re.MULTILINE)
    if not match:
        raise ValueError(f"Missing {name} in CMakeCache.txt")
    return match[1].strip()


def identifier(prefix, relative):
    return prefix + hashlib.sha256(relative.lower().encode("utf-8")).hexdigest()[:32]


def node(parent, tag, **attributes):
    return ET.SubElement(parent, f"{{{NS}}}{tag}", attributes)


def payload_fragment(stage, output):
    """One stable component per installed file; never harvest user data."""
    root = ET.Element(f"{{{NS}}}Wix")
    fragment = node(root, "Fragment")
    directories = {".": node(fragment, "DirectoryRef", Id="INSTALLFOLDER"),
                   "bin": node(fragment, "DirectoryRef", Id="BINFOLDER")}
    group = node(fragment, "ComponentGroup", Id="CorePayload")
    manifest = []
    for file in sorted(stage.rglob("*")):
        if not file.is_file():
            continue
        relative = file.relative_to(stage).as_posix()
        parent = file.parent.relative_to(stage)
        for directory in reversed((parent, *parent.parents)):
            key = directory.as_posix()
            if key in directories:
                continue
            directories[key] = node(directories[directory.parent.as_posix()], "Directory",
                                    Id=identifier("D", key),
                                    Name=directory.name)
        component_id = identifier("C", relative)
        component = node(directories[parent.as_posix()], "Component",
                         Id=component_id, Guid="*", Bitness="always64")
        gui = relative == "bin/connectcoin-qt.exe"
        huge_pages = relative == f"bin/{HUGE_PAGES_HELPER}.exe"
        file_id = "GuiExe" if gui else "HugePagesExe" if huge_pages else identifier("F", relative)
        element = node(component, "File", Id=file_id,
                       Source=str(file), KeyPath="yes")
        if gui:
            for shortcut_id, directory in (("StartMenuShortcut", "CoreMenuFolder"),
                                           ("DesktopShortcut", "DesktopFolder")):
                shortcut = node(element, "Shortcut", Id=shortcut_id, Directory=directory,
                                Name="ConnectCoin Core", Advertise="yes", Icon="CoreIcon.exe",
                                IconIndex="0", WorkingDirectory="BINFOLDER")
                node(shortcut, "ShortcutProperty", Key="System.AppUserModel.ID",
                     Value="ConnectCoin.Core")
        elif huge_pages:
            shortcut = node(element, "Shortcut", Id="HugePagesShortcut", Directory="CoreMenuFolder",
                            Name="Configure Huge Pages", Arguments="configure", Advertise="yes",
                            Icon="CoreIcon.exe", IconIndex="0", WorkingDirectory="BINFOLDER")
            node(shortcut, "ShortcutProperty", Key="System.AppUserModel.ID",
                 Value="ConnectCoin.Core.HugePages")
        node(group, "ComponentRef", Id=component_id)
        manifest.append({"path": relative, "size": file.stat().st_size,
                         "sha256": hashlib.sha256(file.read_bytes()).hexdigest()})
    ET.indent(root)
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)
    return manifest


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO / "build")
    parser.add_argument("--wix", type=Path, required=True, help="WiX 5 wix.exe")
    parser.add_argument("--ui-extension", type=Path, required=True,
                        help="Matching WixToolset.UI.wixext.dll")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--package-version", help="MSI version: major.minor.build (must increase for updates)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--skip-build", action="store_true", help="Package already-built Release targets")
    options = parser.parse_args()
    if os.name != "nt":
        parser.error("Run this script on Windows with an MSVC x64-windows build.")
    build = options.build_dir.resolve(strict=True)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if Path(cache_value(cache, "CMAKE_HOME_DIRECTORY")).resolve() != REPO:
        parser.error("The build directory belongs to a different source tree.")
    if cache_value(cache, "VCPKG_TARGET_TRIPLET") != "x64-windows":
        parser.error("This packager currently requires the dynamic x64-windows triplet.")
    core_version = cache_value(cache, "CMAKE_PROJECT_VERSION")
    version = options.package_version or core_version
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        parser.error("MSI version must have exactly three integer fields.")
    if any(int(part) > limit for part, limit in zip(version.split("."), (255, 255, 65535))):
        parser.error("MSI version exceeds Windows Installer limits.")
    wix = options.wix.resolve(strict=True)
    extension = options.ui_extension.resolve(strict=True)
    wix_version = subprocess.check_output([str(wix), "--version"], text=True).strip()
    if not wix_version.startswith("5."):
        parser.error("Use WiX 5.x with its matching UI extension; newer versions have different licensing terms.")
    output = (options.output_dir or build / "msi").resolve()
    output.mkdir(parents=True, exist_ok=True)
    msi = output / f"connectcoin-core-{version}-win64.msi"
    if msi.exists():
        parser.error(f"Refusing to overwrite {msi}; use a new output directory/version.")
    # A fresh staging directory avoids stale files and needs no recursive deletion.
    work = Path(tempfile.mkdtemp(prefix="msi-work-", dir=build))
    stage = work / "payload"
    if not options.skip_build:
        run("cmake", "--build", build, "--config", "Release", "--target", *TARGETS,
            "--parallel", options.jobs)
    # Advertised shortcut icons must be PE resources, with an .exe identifier
    # matching the target's extension. A tiny resource-only PE avoids duplicating
    # the entire GUI executable in the MSI's uncompressed Icon table.
    run("cmake", "--build", build, "--config", "Release", "--target", "connectcoin-msi-icon", HUGE_PAGES_HELPER,
        "--parallel", options.jobs)
    shortcut_icon = build / "msi-resources/Release/connectcoin-msi-icon.exe"
    if not shortcut_icon.is_file():
        raise RuntimeError(f"Missing Windows shortcut icon resource: {shortcut_icon}")
    for target in TARGETS:
        run("cmake", "--install", build, "--config", "Release", "--component", target,
            "--prefix", stage)
    binaries = stage / "bin"
    copy_file(build / "msi-resources/Release" / f"{HUGE_PAGES_HELPER}.exe",
              binaries / f"{HUGE_PAGES_HELPER}.exe")
    for filename in (*[f"{t}.exe" for t in TARGETS], "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                     "platforms/qwindows.dll", "styles/qmodernwindowsstyle.dll"):
        if not (binaries / filename).is_file():
            raise RuntimeError(f"Required payload missing: {filename}")
    unexpected = {p.name for p in binaries.glob("*.exe")} - {f"{t}.exe" for t in (*TARGETS, HUGE_PAGES_HELPER)}
    if unexpected or (binaries / "Qt6Test.dll").exists():
        raise RuntimeError(f"Unexpected test/development payload: {unexpected}")

    # Use the compiler's own redistributable directory, never Windows/System32.
    compilers = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    compiler_text = compilers[-1].read_text(encoding="utf-8") if compilers else ""
    match = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+/VC)/Tools/MSVC/([^/]+)/', compiler_text)
    if not match:
        raise RuntimeError("Cannot locate the MSVC compiler's redistributable directory.")
    crt_dirs = list((Path(match[1]) / "Redist/MSVC" / match[2] / "x64").glob("Microsoft.VC*.CRT"))
    if len(crt_dirs) != 1:
        raise RuntimeError("Install the compiler-matching Visual C++ redistributables in Visual Studio.")
    for file in crt_dirs[0].glob("*.dll"):
        copy_file(file, binaries / file.name)
    for filename in ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                     "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        if not (binaries / filename).is_file():
            raise RuntimeError(f"Missing Visual C++ runtime: {filename}")

    for source, destination in (("COPYING", "COPYING.txt"),
                                ("contrib/windeploy/MSI-README.txt", "README.txt"),
                                ("doc/assets-attribution.md", "doc/assets-attribution.md"),
                                ("contrib/debian/copyright", "contrib/debian/copyright"),
                                ("share/examples/connectcoin.conf", "share/examples/connectcoin.conf"),
                                ("share/rpcauth/rpcauth.py", "share/rpcauth/rpcauth.py")):
        copy_file(REPO / source, stage / destination)
    for name, license_path in (("RandomX", "src/randomx/LICENSE"), ("secp256k1", "src/secp256k1/COPYING"),
                               ("leveldb", "src/leveldb/LICENSE"), ("crc32c", "src/crc32c/LICENSE"),
                               ("minisketch", "src/minisketch/LICENSE"), ("ctaes", "src/crypto/ctaes/COPYING")):
        copy_file(REPO / license_path, stage / "licenses" / f"{name}.txt")
    copy_file(build / "_deps/connectcoin_mbedtls-src/LICENSE", stage / "licenses/mbedtls.txt")
    for name in ("p256-m", "everest"):
        copy_file(build / f"_deps/connectcoin_mbedtls-src/3rdparty/{name}/README.md",
                  stage / "licenses" / f"mbedtls-{name}.md")
    (stage / "licenses/UniValue.txt").write_text(
        "UniValue\nCopyright 2014 BitPay Inc.\nCopyright 2015 Bitcoin Core Developers\n\n"
        + (REPO / "COPYING").read_text(encoding="utf-8"), encoding="utf-8")
    vcpkg = Path(cache_value(cache, "VCPKG_INSTALLED_DIR")) / "x64-windows"
    for license_file in sorted((vcpkg / "share").glob("*/copyright")):
        copy_file(license_file, stage / "licenses" / f"vcpkg-{license_file.parent.name}.txt")
    # Include provenance useful when redistributing/rebuilding the bundled LGPL Qt DLLs.
    provenance = {"core_version": core_version, "msi_version": version, "wix": wix_version,
                  "installer_language": "en-US",
                  "git_commit": subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip(),
                  "working_tree_dirty": bool(subprocess.check_output(
                      ["git", "-C", str(REPO), "status", "--porcelain"], text=True).strip()),
                  "qt_version": re.search(r'set\(PACKAGE_VERSION "([^"]+)"',
                      (vcpkg / "share/Qt6Core/Qt6CoreConfigVersionImpl.cmake").read_text())[1],
                  "vcpkg_build_dependencies": (vcpkg.parent / "vcpkg/status").read_text(encoding="utf-8")}
    (stage / "build-info.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    license_rtf = work / "license.rtf"
    mit = (REPO / "COPYING").read_text(encoding="utf-8")
    escaped = mit.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}").replace("\n", "\\par\n")
    license_rtf.write_text("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\f0\\fs20 " + escaped + "}", encoding="ascii")
    fragment = work / "payload.wxs"
    manifest = payload_fragment(stage, fragment)
    run(wix, "build", REPO / "contrib/windeploy/core.wxs", fragment,
        "-arch", "x64", "-culture", "en-us", "-ext", extension,
        "-d", f"PackageVersion={version}", "-d", f"CoreVersion={core_version}",
        "-d", f"ShortcutIcon={shortcut_icon}", "-d", f"LicenseRtf={license_rtf}",
        "-intermediatefolder", work / "wix", "-o", msi)
    (output / f"{msi.stem}.manifest.json").write_text(json.dumps({"build": provenance, "files": manifest}, indent=2) + "\n", encoding="utf-8")
    (output / f"{msi.name}.sha256").write_text(f"{hashlib.sha256(msi.read_bytes()).hexdigest()}  {msi.name}\n", encoding="ascii")
    print(f"MSI: {msi}\nStaging and build diagnostics: {work}", flush=True)


if __name__ == "__main__":
    main()
