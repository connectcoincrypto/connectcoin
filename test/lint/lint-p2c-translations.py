# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Validate P2C catalog coverage and formatting without rewriting inherited text."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
QT = ROOT / "src/qt"
EXTRA_MESSAGES = {
    "BitcoinGUI": {"&P2C", "Create pay-to-connect bounties"},
    "TransactionView": {"Enter address, P2C domain, transaction id, or label to search"},
    "TransactionTableModel": {"P2C: %1", "User-defined intent/purpose of the transaction, or the P2C domain."},
    "TransactionDesc": {"P2C domain"},
}


def required_messages():
    result = {name: set(messages) for name, messages in EXTRA_MESSAGES.items()}
    for name, filename in (("P2CCreateDialog", "p2ccreatedialog.cpp"), ("P2CClaimDialog", "p2cclaimdialog.cpp")):
        # These dialogs use standalone C++ string literals, whose escapes here
        # are also valid JSON escapes. Decode newlines before comparing with TS.
        source = (QT / filename).read_text(encoding="utf-8")
        result[name] = {json.loads('"' + value + '"') for value in re.findall(r'\btr\("((?:[^"\\]|\\.)*)"\)', source)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locale", action="append", help="Check a specific locale (default: all bundled locales)")
    args = parser.parse_args()
    required = required_messages()
    errors = []
    catalogs = ET.parse(QT / "bitcoin_locale.qrc").getroot().findall(".//file")
    if args.locale:
        unknown = set(args.locale) - {resource.get("alias") for resource in catalogs}
        if unknown:
            parser.error(f"Unknown locales: {sorted(unknown)}")
        catalogs = [resource for resource in catalogs if resource.get("alias") in args.locale]
    for resource in catalogs:
        path = QT / Path(resource.text).with_suffix(".ts")
        root = ET.parse(path).getroot()
        translated = {}
        for context in root.findall("context"):
            name = context.findtext("name")
            if name not in required:
                continue
            for message in context.findall("message"):
                source = message.findtext("source")
                if source not in required[name]:
                    continue
                key = (name, source)
                if key in translated:
                    errors.append(f"{path.name}: duplicate {key}")
                translated[key] = message.find("translation")
        for name, messages in required.items():
            for source in sorted(messages):
                translation = translated.get((name, source))
                label = f"{path.name}: {name}: {source!r}"
                if translation is None or translation.get("type") in ("unfinished", "vanished", "obsolete") or not translation.text:
                    errors.append(f"{label}: missing completed translation")
                    continue
                text = translation.text
                if Counter(re.findall(r"%[1-9][0-9]*|%n", source)) != Counter(re.findall(r"%[1-9][0-9]*|%n", text)):
                    errors.append(f"{label}: changed placeholders")
                if Counter(re.findall(r"</?\w+\s*/?>", source)) != Counter(re.findall(r"</?\w+\s*/?>", text)):
                    errors.append(f"{label}: changed rich-text markup")
                elif "<br" in source:
                    try:
                        ET.fromstring("<root>" + text + "</root>")
                    except ET.ParseError:
                        errors.append(f"{label}: malformed rich text")
                if source.count("&") != text.count("&"):
                    errors.append(f"{label}: changed keyboard mnemonic count")
                if source.count("\n") != text.count("\n"):
                    errors.append(f"{label}: changed line breaks")
                for token in ("P2C", "TLS", "HTTPS", "ASCII", "IP", "URL", "example.com", "another.example"):
                    if source.count(token) != text.count(token):
                        errors.append(f"{label}: changed technical identifier {token}")
    # A new worker state must get a GUI translation, never silently fall back.
    worker = (ROOT / "src/wallet/p2c_worker_impl.cpp").read_text(encoding="utf-8")
    states = set(re.findall(r'Message\("([^"\n]+)"', worker))
    states.update(re.findall(r'(?:state\s*=|state\{)\s*"([^"\n]+)"', worker))
    states.update({"starting", "submitted", "stored; check wallet history"})
    mapped = set(re.findall(r'if \(state == "([^"\n]+)"\)', (QT / "p2cclaimdialog.cpp").read_text(encoding="utf-8")))
    for state in sorted(states - mapped):
        errors.append(f"Untranslated P2C worker state: {state}")
    if errors:
        print("\n".join(errors))
        return 1
    print(f"P2C translations: {len(catalogs)} catalogs, {sum(map(len, required.values()))} messages each; formatting and state mappings valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
