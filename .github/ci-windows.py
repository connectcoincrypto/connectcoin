#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

def run(cmd, **kwargs):
    print("+ " + shlex.join(cmd), flush=True)
    kwargs.setdefault("check", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


GENERATE_OPTIONS = {
    "standard": [
        "-DBUILD_BENCH=ON",
        "-DBUILD_KERNEL_LIB=ON",
        "-DBUILD_UTIL_CHAINSTATE=ON",
        "-DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
    ],
    "fuzz": [
        "-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON",
        "-DVCPKG_MANIFEST_FEATURES=wallet",
        "-DBUILD_FOR_FUZZING=ON",
        "-DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
    ],
}


def vcpkg_cache_fingerprint(archives):
    """Hash completed binary archives, ignoring transient vcpkg files."""
    digest = hashlib.sha256()
    for archive in sorted(archives.rglob("*.zip")):
        digest.update(archive.relative_to(archives).as_posix().encode("utf8"))
        digest.update(b"\0")
        with archive.open("rb") as archive_file:
            digest.update(hashlib.file_digest(archive_file, "sha256").digest())
    return digest.hexdigest()


def vcpkg_cache_state(_ci_type):
    archives = Path(os.environ["LOCALAPPDATA"]) / "vcpkg" / "archives"
    fingerprint = vcpkg_cache_fingerprint(archives)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf8") as output:
        output.write(f"fingerprint={fingerprint}\n")


def github_import_vs_env(_ci_type):
    vswhere_path = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    installation_path = run(
        [str(vswhere_path), "-latest", "-property", "installationPath"],
        capture_output=True,
        text=True,
    ).stdout.strip()
    vsdevcmd = Path(installation_path) / "Common7" / "Tools" / "vsdevcmd.bat"
    comspec = os.environ["COMSPEC"]
    output = run(
        f'"{comspec}" /s /c ""{vsdevcmd}" -arch=x64 -no_logo && set"',
        capture_output=True,
        text=True,
    ).stdout
    github_env = os.environ["GITHUB_ENV"]
    with open(github_env, "a") as env_file:
        for line in output.splitlines():
            if "=" not in line:
                continue
            name, value = line.split("=", 1)
            env_file.write(f"{name}={value}\n")


def setup_vcpkg(_ci_type):
    # The manifest pins ports, but not the tool and scripts provided by the
    # runner image (or overwritten by VsDevCmd). Pin those inputs as well.
    manifest = json.loads(Path("vcpkg.json").read_text(encoding="utf8"))
    baseline = manifest["builtin-baseline"]
    if not isinstance(baseline, str) or not re.fullmatch(r"[0-9a-f]{40}", baseline):
        raise ValueError("vcpkg builtin-baseline must be a full commit hash")
    vcpkg_root = Path(os.environ["RUNNER_TEMP"]) / "connectcoin-ci-vcpkg"
    # A fresh, job-owned checkout avoids modifying any installed vcpkg tree.
    vcpkg_root.mkdir()
    run(["git", "init", str(vcpkg_root)])
    run(["git", "-C", str(vcpkg_root), "fetch", "--depth=1",
         "https://github.com/microsoft/vcpkg.git", baseline])
    run(["git", "-C", str(vcpkg_root), "checkout", "--detach", baseline])
    # Keep the batch-file name free of path quoting: cwd may contain spaces.
    run([os.environ["COMSPEC"], "/d", "/c", "bootstrap-vcpkg.bat", "-disableMetrics"], cwd=vcpkg_root)
    run([str(vcpkg_root / "vcpkg.exe"), "version"])
    print(f"Pinned vcpkg scripts to manifest baseline {baseline}", flush=True)
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf8") as env_file:
        env_file.write(f"VCPKG_ROOT={vcpkg_root}\n")


def generate(ci_type):
    command = [
        "cmake",
        "-B",
        "build",
        "-Werror=dev",
        "--preset=vs2026",
        # Using x64-windows-release for both host and target triplets
        # to ensure vcpkg builds only release packages, thereby optimizing
        # build time.
        # See https://github.com/microsoft/vcpkg/issues/50927.
        "-DVCPKG_HOST_TRIPLET=x64-windows-release",
        "-DVCPKG_TARGET_TRIPLET=x64-windows-release",
    ] + GENERATE_OPTIONS[ci_type]
    if run(command, check=False).returncode != 0:
        print("=== ⚠️ ===")
        print("Generate failure! Network issue? Retry once ...")
        time.sleep(12)
        print("=== ⚠️ ===")
        run(command)


def build(_ci_type):
    command = [
        "cmake",
        "--build",
        "build",
        "--config",
        "Release",
    ]
    if run(command + ["-j", str(os.process_cpu_count())], check=False).returncode != 0:
        print("Build failure. Verbose build follows.")
        run(command + ["-j1", "--verbose"])


def check_manifests(ci_type):
    if ci_type != "standard":
        print(f"Skipping manifest validation for '{ci_type}' ci type.")
        return

    release_dir = Path.cwd() / "build" / "bin" / "Release"
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
        cmd_check_manifest = [
            "mt.exe",
            "-nologo",
            f"-inputresource:{entry}",
            "-validate_manifest",
        ]
        run(cmd_check_manifest)


def prepare_tests(ci_type):
    workspace = Path.cwd()
    if ci_type == "standard":
        run([sys.executable, "-m", "pip", "install", "pyzmq"])
    elif ci_type == "fuzz":
        run([sys.executable, str(workspace / "ci" / "test" / "qa_assets.py"), str(workspace / "qa-assets")])


def run_tests(ci_type):
    workspace = Path.cwd()
    build_dir = workspace / "build"
    num_procs_int = os.process_cpu_count() or 1
    num_procs = str(num_procs_int)
    release_bin = build_dir / "bin" / "Release"

    if ci_type == "standard":
        test_envs = {
            "CONNECTCOIN_BIN": "connectcoin.exe",
            "CONNECTCOIND": "connectcoind.exe",
            "CONNECTCOINCLI": "connectcoin-cli.exe",
            "CONNECTCOIN_BENCH": "connectcoin-bench.exe",
            "CONNECTCOINTX": "connectcoin-tx.exe",
            "CONNECTCOINUTIL": "connectcoin-util.exe",
            "CONNECTCOINWALLET": "connectcoin-wallet.exe",
            "CONNECTCOINCHAINSTATE": "connectcoin-chainstate.exe",
        }
        for var, exe in test_envs.items():
            os.environ[var] = str(release_bin / exe)

        ctest_cmd = [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--stop-on-failure",
            "-j",
            num_procs,
            "--build-config",
            "Release",
        ]
        # Preserve full real-PoW coverage in the dedicated suites, even if a
        # calling environment requested the reduced profile. Run them serially
        # to avoid competing dataset initialization on the Windows runner.
        real_pow_env = os.environ.copy()
        real_pow_env.pop("TEST_RANDOMX_MOCK_POW", None)
        run(ctest_cmd + ["-j", "1", "-R", "^(pow_tests|randomx_tests)$", "--no-tests=error"], env=real_pow_env)

        # Generic fixtures and functional tests exercise chain behavior, not
        # RandomX itself. Match the other CI platforms' reduced test profile so
        # each unrelated CTest process does not initialize a multi-GiB dataset.
        os.environ["TEST_RANDOMX_MOCK_POW"] = "1"
        run(ctest_cmd)

        test_cmd = [
            sys.executable,
            str(build_dir / "test" / "functional" / "test_runner.py"),
            "--jobs",
            num_procs,
            "--quiet",
            f"--tmpdirprefix={workspace / '_ _'}",
            "--combinedlogslen=99999999",
            *shlex.split(os.environ.get("TEST_RUNNER_EXTRA", "").strip()),
        ]
        run(test_cmd)

    elif ci_type == "fuzz":
        os.environ["CONNECTCOINFUZZ"] = str(release_bin / "fuzz.exe")
        fuzz_cmd = [
            sys.executable,
            str(build_dir / "test" / "fuzz" / "test_runner.py"),
            "--par",
            num_procs,
            "--corpus-shards",
            str(num_procs_int * 4),
            "--loglevel",
            "DEBUG",
            str(workspace / "qa-assets" / "fuzz_corpora"),
        ]
        run(fuzz_cmd)


def main():
    parser = argparse.ArgumentParser(description="Utility to run Windows CI steps.")
    parser.add_argument("ci_type", choices=GENERATE_OPTIONS, help="CI type to run.")
    steps = list(map(lambda f: f.__name__, [
        github_import_vs_env,
        setup_vcpkg,
        vcpkg_cache_state,
        generate,
        build,
        check_manifests,
        prepare_tests,
        run_tests,
    ]))
    parser.add_argument("step", choices=steps, help="CI step to perform.")
    args = parser.parse_args()

    exec(f'{args.step}("{args.ci_type}")')


if __name__ == "__main__":
    main()
