# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Read the release package version without configuring a compiler or CMake."""

import argparse
from pathlib import Path
import re


def package_version(source):
    """Accept one literal assignment per version component; never evaluate CMake."""
    components = {}
    source = re.sub(r"(?m)#.*$", "", source)
    assignment = re.compile(r"\b(?i:set)\s*\(\s*(CLIENT_VERSION_(?:MAJOR|MINOR|BUILD|RC))\b")
    for match in assignment.finditer(source):
        name = match[1]
        if name in components:
            raise ValueError(f"Duplicate version definition: {name}")
        # Version metadata is deliberately restricted to the canonical one-line
        # decimal definitions. Expressions and additional CMake commands fail
        # closed instead of silently producing a different package version.
        tail = source[match.end():].split("\n", 1)[0]
        value = re.fullmatch(r'[ \t]+(0|[1-9][0-9]*)[ \t]*\)[ \t\r]*', tail)
        if value is None:
            raise ValueError(f"Expected a literal decimal version definition: {name}")
        components[name] = int(value[1])
    for part in ("MAJOR", "MINOR", "BUILD", "RC"):
        if f"CLIENT_VERSION_{part}" not in components:
            raise ValueError(f"Missing version definition: CLIENT_VERSION_{part}")
    if components["CLIENT_VERSION_RC"]:
        raise ValueError("Release installers require CLIENT_VERSION_RC=0")
    return ".".join(str(components[f"CLIENT_VERSION_{part}"]) for part in ("MAJOR", "MINOR", "BUILD"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2] / "CMakeLists.txt")
    args = parser.parse_args()
    try:
        print(package_version(args.source.read_text(encoding="utf-8")))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
