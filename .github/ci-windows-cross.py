#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

import argparse
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

def run(cmd, **kwargs):
    print("+ " + shlex.join(cmd), flush=True)
    kwargs.setdefault("check", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


def print_version():
    connectcoind = Path.cwd() / "bin" / "connectcoind.exe"
    run([str(connectcoind), "-version"])


def check_imports():
    connectcoind = Path.cwd() / "bin" / "connectcoind.exe"
    output = run(
        ["dumpbin.exe", "/imports", str(connectcoind)],
        capture_output=True,
        text=True,
    ).stdout
    dlls = re.findall(r"^\s*(\S+\.dll)\s*$", output, re.IGNORECASE | re.MULTILINE)
    print("\n".join(dlls))

    # Ensure the executable is linked against the expected C runtime.
    dlls = {name.lower() for name in dlls}
    uses_msvcrt = "msvcrt.dll" in dlls
    uses_ucrt = any(name.startswith("api-ms-win-crt-") for name in dlls)
    crt = os.environ["CRT"]
    if crt == "msvcrt":
        crt_ok = uses_msvcrt and not uses_ucrt
    elif crt == "ucrt":
        crt_ok = uses_ucrt and not uses_msvcrt
    else:
        sys.exit(f"Unexpected CRT value: {crt!r}")
    if not crt_ok:
        sys.exit(f"Imported DLLs do not match the expected {crt!r} C runtime.")


def check_manifests():
    release_dir = Path.cwd() / "bin"
    manifest_path = release_dir / "connectcoind.manifest"

    cmd_connectcoind_manifest = [
        "mt.exe",
        "-nologo",
        f"-inputresource:{release_dir / 'connectcoind.exe'}",
        f"-out:{manifest_path}",
    ]
    run(cmd_connectcoind_manifest)
    print(manifest_path.read_text())

    for entry in release_dir.iterdir():
        if entry.suffix.lower() != ".exe":
            continue
        print(f"Checking {entry.name}")
        run(["mt.exe", "-nologo", f"-inputresource:{entry}", "-validate_manifest"])


def prepare_tests():
    workspace = Path.cwd()
    config_path = workspace / "test" / "config.ini"
    rpcauth_path = workspace / "share" / "rpcauth" / "rpcauth.py"
    replacements = {
        "SRCDIR=": f"SRCDIR={workspace}",
        "BUILDDIR=": f"BUILDDIR={workspace}",
        "RPCAUTH=": f"RPCAUTH={rpcauth_path}",
    }
    lines = config_path.read_text().splitlines()
    for index, line in enumerate(lines):
        for prefix, new_value in replacements.items():
            if line.startswith(prefix):
                lines[index] = new_value
                break
    content = "\n".join(lines) + "\n"
    config_path.write_text(content)
    print(content)
    run([sys.executable, "-m", "pip", "install", "pyzmq"])


def run_functional_tests():
    workspace = Path.cwd()
    num_procs = str(os.process_cpu_count())
    test_runner_cmd = [
        sys.executable,
        str(workspace / "test" / "functional" / "test_runner.py"),
        "--jobs",
        num_procs,
        "--quiet",
        f"--tmpdirprefix={workspace / '_ _'}",
        "--combinedlogslen=99999999",
        *shlex.split(os.environ.get("TEST_RUNNER_EXTRA", "").strip()),
    ]
    run(test_runner_cmd)


def run_unit_tests():
    # Can't use ctest here like other jobs as we don't have a CMake build tree.
    commands = [
        ["./bin/connectcoin-test-qt.exe"],
        # Intentionally run sequentially here, to catch test case failures caused by dirty global state from prior test cases:
        ["./bin/connectcoin-test.exe", "-l", "test_suite"],
        ["./src/secp256k1/bin/exhaustive_tests.exe"],
        ["./src/secp256k1/bin/noverify_tests.exe"],
        ["./src/secp256k1/bin/tests.exe"],
        ["./src/univalue/object.exe"],
        ["./src/univalue/unitester.exe"],
    ]
    for cmd in commands:
        run(cmd)


def main():
    parser = argparse.ArgumentParser(description="Utility to run Windows CI steps.")
    steps = list(map(lambda f: f.__name__, [
        print_version,
        check_imports,
        check_manifests,
        prepare_tests,
        run_unit_tests,
        run_functional_tests,
    ]))
    parser.add_argument("step", choices=steps, help="CI step to perform.")
    args = parser.parse_args()

    exec(f'{args.step}()')


if __name__ == "__main__":
    main()
