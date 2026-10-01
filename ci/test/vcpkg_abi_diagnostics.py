# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Record selected vcpkg ABI inputs, never environment variables or build logs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import zipfile
import zlib


MAX_ABI_BYTES = 256 * 1024


def abi_entries(data):
    entries = {}
    for line in data.decode("utf8").splitlines():
        key, separator, value = line.partition(" ")
        if not separator:
            continue
        # Port/dependency/compiler inputs are hashes. Limit the few unhashed
        # fields to known non-sensitive values; never copy arbitrary metadata.
        is_hash = re.fullmatch(r"[0-9a-f]{40,128}(?:-[0-9a-f]{40,128})*", value)
        is_version = key in {"cmake", "powershell", "post_build_checks", "sbom_info"} and re.fullmatch(r"[0-9.]+", value)
        is_label = key in {"features", "triplet"} and re.fullmatch(r"[a-z0-9,;_+\-]+", value)
        if is_hash or is_version or is_label:
            entries[key] = value
    return entries


def collect(archives, buildtrees=None):
    report = {"archive_count": 0, "archive_bytes": 0, "cached": [], "current": [], "errors": []}
    try:
        archive_paths = sorted(archives.rglob("*.zip"))
    except OSError:
        archive_paths = []
        report["errors"].append("unreadable cache directory")
    for archive in archive_paths:
        try:
            if archive.is_symlink():
                continue
            size = archive.stat().st_size
            report["archive_count"] += 1
            report["archive_bytes"] += size
            if not re.fullmatch(r"[0-9a-f]{64}", archive.stem):
                raise ValueError("invalid archive identity")
            with zipfile.ZipFile(archive) as package:
                for member in package.infolist():
                    match = re.fullmatch(r"share/([a-z0-9-]+)/vcpkg_abi_info\.txt", member.filename)
                    if match and member.file_size <= MAX_ABI_BYTES:
                        data = package.read(member)
                        if hashlib.sha256(data).hexdigest() != archive.stem:
                            raise ValueError("archive identity disagrees with ABI metadata")
                        report["cached"].append({
                            "port": match[1], "abi": archive.stem,
                            "entries": abi_entries(data),
                        })
        except (EOFError, OSError, RuntimeError, ValueError, zipfile.BadZipFile, zlib.error):
            # Do not expose exception text: it can contain arbitrary ZIP data.
            report["errors"].append("unreadable cached ABI metadata")
    if buildtrees is not None:
        try:
            current_paths = sorted(buildtrees.glob("*/*.vcpkg_abi_info.txt"))
        except OSError:
            current_paths = []
            report["errors"].append("unreadable buildtrees directory")
        for path in current_paths:
            try:
                if path.is_symlink() or path.stat().st_size > MAX_ABI_BYTES:
                    continue
                data = path.read_bytes()
                report["current"].append({
                    "port": path.parent.name, "abi": hashlib.sha256(data).hexdigest(),
                    "entries": abi_entries(data),
                })
            except (OSError, ValueError):
                report["errors"].append("unreadable current ABI metadata")
    return report


def compare(before, after):
    comparisons = []
    for current in after["current"]:
        candidates = [cached for cached in before["cached"]
                      if cached["port"] == current["port"] and
                      cached["entries"].get("triplet") == current["entries"].get("triplet")]
        exact_match = any(cached["abi"] == current["abi"] for cached in candidates)
        differences = []
        if not exact_match:
            for cached in candidates:
                keys = sorted(set(cached["entries"]) | set(current["entries"]))
                differences.append({"cached_abi": cached["abi"], "changed_inputs": {
                    key: {"cached": cached["entries"].get(key), "current": current["entries"].get(key)}
                    for key in keys if cached["entries"].get(key) != current["entries"].get(key)
                }})
        comparisons.append({"port": current["port"], "abi": current["abi"],
                            "cache_status": "match" if exact_match else "abi_mismatch" if candidates else "not_cached",
                            "differences": differences})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("before", "after"))
    phase = parser.parse_args().phase
    directory = Path("build/ci-vcpkg-abi")
    directory.mkdir(parents=True, exist_ok=True)
    archives = Path(os.environ["LOCALAPPDATA"]) / "vcpkg/archives"
    buildtrees = Path(os.environ["VCPKG_ROOT"]) / "buildtrees" if phase == "after" else None
    report = collect(archives, buildtrees)
    before = directory / "before.json"
    if phase == "after" and before.is_file():
        report["comparison"] = compare(json.loads(before.read_text(encoding="utf8")), report)
    (directory / f"{phase}.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(f"vcpkg {phase}: {report['archive_count']} archives, {report['archive_bytes']} bytes, "
          f"{len(report['current'])} current ABI records, {len(report['errors'])} metadata errors")
    for item in report.get("comparison", []):
        changed = sorted({key for candidate in item["differences"] for key in candidate["changed_inputs"]})
        print(f"  {item['port']}: {item['cache_status']}" + (f"; changed inputs: {', '.join(changed)}" if changed else ""))


if __name__ == "__main__":
    main()
