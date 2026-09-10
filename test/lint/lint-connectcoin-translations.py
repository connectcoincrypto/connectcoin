# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check ConnectCoin GUI translations and report real active-message coverage.

An inherited, nonempty `unfinished` translation is counted as available because
lrelease does not use -nounfinished. This is NOT a claim of native-speaker review.
Missing translations are never counted merely because Qt falls back to English.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
QT = ROOT / "src/qt"
PRINTF_PLACEHOLDERS = re.compile(r"%%|%(?:[1-9][0-9]*\$)?[-+#0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]")
QT_PLACEHOLDERS = re.compile(r"%L?(?:[1-9][0-9]*|n)")
# Qt rich-text tags, excluding literal help metavariables such as <amount> and
# <wallet name>, which may legitimately be translated in explanatory text.
MARKUP = re.compile(r"</?(?:a|address|b|big|blockquote|body|br|caption|center|cite|code|dd|dfn|div|dl|dt|em|font|h[1-6]|head|hr|html|i|img|kbd|li|link|meta|nobr|ol|p|pre|qt|s|samp|small|span|strike|strong|style|sub|sup|table|tbody|td|tfoot|th|thead|title|tr|tt|u|ul|var)(?=[\s/>])[^<>]*>", re.IGNORECASE)
COMMAND_FLAGS = re.compile(r"(?<![\w/])--?[A-Za-z][A-Za-z0-9_-]*")
# These are functional literals, not prose: changing a URI scheme misdirects
# users, and QFileDialog interprets the ASCII parenthesized wildcard itself.
CONNECTCOIN_URIS = re.compile(r"(?<![A-Za-z0-9_+.-])connectcoin:/*")
QUOTED_CONNECTCOIN_URI = re.compile(r"['\"]connectcoin:/*['\"]")
FILE_FILTERS = re.compile(r"\([^()]*\*\.[^()]*\)")
VOID_TAGS = {"br", "hr", "img", "link", "meta"}
# Counts produced by Qt Linguist 6.11.1 for the exact language identifiers in
# the bundled TS files, not inferred from their display names. Keep in sync
# when changing a catalog's language tag or adopting different Qt plural rules.
PLURAL_FORMS = {
    locale: count for count, names in {
        1: "ast_ES az@latin cmn fa hak hu id ja kk@latin ko ms pam sr@ijekavianlatin sr@latin szl th tr uz@Cyrl uz@Latn ve yo yue zh-Hans zh-Hant zh zh_CN zh_HK zh_TW",
        2: "am az bg bn br ca da de el en eo es et eu fi fil fo fr gl gl_ES gu he hi is it ka kk km kn ku ku_IQ ml mn nb ne nl no ps pt pt_BR si sq sv sw ta te tk tl ur uz yi",
        3: "be bs cs ga ga_IE hr lt lv mi mk pl ro ru sk sm sr uk",
        4: "mt sl",
        5: "cy",
        6: "ar",
    }.items() for locale in names.split()
}
MANDATORY = {
    ("BitcoinGUI", "&Mining", "", ""),
    ("BitcoinGUI", "Control CPU mining", "", ""),
    ("OptionsDialog", "Enable pop-up notifications", "", ""),
    ("OptionsDialog", "Show desktop pop-up notifications, including incoming and sent transactions. Disabled by default. Error and confirmation dialogs remain enabled.", "", ""),
}
CORE_MESSAGES = {
    "Change destination must be a type-1 P2PK (bech32m) destination",
    "ConnectCoin supports only type-1 P2PK (bech32m) addresses",
    "ConnectCoin supports only type-1 P2PK (bech32m) change addresses",
    "ConnectCoin transactions require valid type-1 destinations or type-2 PAY_TO_CONNECT outputs",
    "ConnectCoin type-1 outputs support only SIGHASH_DEFAULT",
    "Input is not a complete type-1 SIGHASH_DEFAULT spend",
    "Input requires a complete P2C proof witness",
    "Mainnet has not been launched: no genesis block is defined. Use -testnet4 for public testing or -regtest for local testing.",
    "No project-owned public source URL is configured for this development build.",
    "Not enough file descriptors available. Try reducing -rpcmaxconnections or using the default value of %d",
    "Please contribute if you find %s useful.",
}
MANDATORY |= {("bitcoin-core", source, "", "") for source in CORE_MESSAGES}
SAFETY_FORMAT = {
    ("AskPassphraseDialog", "Warning: If you encrypt your wallet and lose your passphrase, you will <b>LOSE ALL OF YOUR CONNECTCOIN FUNDS</b>!", "", ""),
    ("BitcoinGUI", "Proxy is <b>enabled</b>: %1", "", ""),
}
# Genuine Dutch wording identical to English, not an untranslated fallback.
SOURCE_IDENTICAL_EXCEPTIONS = {("nl", "Open ConnectCoin URI")}


def read_catalog(path, expected_language=None):
    messages = {}
    root = ET.parse(path).getroot()
    if expected_language is not None and root.get("language") != expected_language:
        raise ValueError(f"{path.name}: unexpected language tag {root.get('language')!r}; recheck Qt plural rules")
    for context in root.findall("context"):
        for message in context.findall("message"):
            key = (context.findtext("name"), message.findtext("source"), message.findtext("comment", ""), message.get("numerus", ""))
            if key in messages:
                raise ValueError(f"{path.name}: duplicate message {key!r}")
            messages[key] = message.find("translation")
    return messages


def forms(translation):
    if translation is None or translation.get("type") in ("obsolete", "vanished"):
        return []
    plurals = translation.findall("numerusform")
    return [(part.text or "") for part in plurals] if plurals else [translation.text or ""]


def available(translation, expected_forms=None, numerus=None):
    values = forms(translation)
    return (bool(values) and all(value.strip() for value in values)
            and (expected_forms is None or len(values) == expected_forms)
            and (numerus is None or bool(translation.findall("numerusform")) == numerus))


def placeholders_match(context, source, translated):
    if context != "bitcoin-core":
        return Counter(QT_PLACEHOLDERS.findall(source)) == Counter(QT_PLACEHOLDERS.findall(translated))
    original = PRINTF_PLACEHOLDERS.findall(source)
    updated = PRINTF_PLACEHOLDERS.findall(translated)
    # Non-positional printf/tinyformat arguments are consumed in sequence.
    # Only explicitly indexed arguments can be freely reordered.
    if all(token == "%%" or re.match(r"%[1-9][0-9]*\$", token) for token in original + updated):
        return Counter(original) == Counter(updated)
    return original == updated


def balanced_markup(text):
    """Check nesting of Qt rich text, not literal help metavariables."""
    stack = []
    for tag in MARKUP.findall(text):
        name = re.match(r"</?([A-Za-z0-9]+)", tag).group(1).lower()
        if tag.startswith("</"):
            if not stack or stack.pop() != name:
                return False
        elif name not in VOID_TAGS and not tag.rstrip().endswith("/>"):
            stack.append(name)
    return not stack


def command_flag_pattern(known_flags):
    """Prefer the longest known option, allowing hyphenated linguistic suffixes.

    The inventory comes from the whole English catalog: a -reindex-chainstate
    occurrence must never count as -reindex, even when only the latter occurs
    in this particular source message. Suffixes such as German '-Option' and
    Finnish '-asetusta' are prose, not new option names. ASCII boundaries also
    exclude Maltese 'l-proxy' while allowing adjacent Chinese or Hebrew prose.
    """
    names = "|".join(re.escape(flag) for flag in sorted(known_flags, key=lambda flag: (-len(flag), flag)))
    # Capture in a lookahead so a failed right boundary cannot backtrack from
    # a longer known name to a shorter one, e.g. -reindex-chainstateTYPO must
    # not be accepted as -reindex plus a linguistic suffix.
    return re.compile(r"(?<![A-Za-z0-9_/-])(?=(?P<flag>" + names + r"))(?P=flag)(?![A-Za-z0-9_])") if names else re.compile(r"(?!)")


def literal_flag_values(tail):
    """Read literal =values, permitting translated metavariables and ellipses.

    For example, -bind=...=onion retains 'onion', whereas -dumpfile=<filename>
    does not freeze the explanatory word 'filename'. Placeholders are checked
    separately. Sentence punctuation and a trailing linguistic joining hyphen
    (German 'onion- und ... Einstellungen', Mongolian '1-ийг') are not values.
    """
    values = []
    while tail.startswith("="):
        tail = tail[1:]
        if tail.startswith(("'", '"')):
            quote = tail[0]
            end = tail.find(quote, 1)
            if end < 0:
                break
            value, tail = tail[1:end], tail[end + 1:]
            if not re.fullmatch(r"<[^<>]*>|\.{3}|…", value) and not (
                    PRINTF_PLACEHOLDERS.fullmatch(value) or QT_PLACEHOLDERS.fullmatch(value)):
                values.append(value)
        elif variable := re.match(r"<[^<>]*>|\.{3}|…", tail):
            tail = tail[variable.end():]
        elif variable := PRINTF_PLACEHOLDERS.match(tail) or QT_PLACEHOLDERS.match(tail):
            tail = tail[variable.end():]
        elif literal := re.match(r"[-A-Za-z0-9_][-A-Za-z0-9_./:\\]*", tail):
            values.append(literal[0].rstrip(".:-"))
            tail = tail[literal.end():]
        else:
            break
    return tuple(values)


def command_flag_tokens(text, pattern):
    return Counter((match[0], literal_flag_values(text[match.end():])) for match in pattern.finditer(text))


def untranslated_sentence(source, translated):
    """Detect copied prose, including short storage plurals, not cognate labels.

    Compare all Unicode text (not just the English letters left in a translated
    sentence). Keep short labels, units and protocol names out of this heuristic.
    It cannot establish whether a differently worded translation is correct.
    """
    def normalized(text):
        text = re.sub(r"<[^>]+>", " ", text)
        text = PRINTF_PLACEHOLDERS.sub(" ", text)
        text = QT_PLACEHOLDERS.sub(" ", text)
        return re.findall(r"\w+", text.casefold())
    original = normalized(source)
    return sum(not token.isdigit() for token in original) >= 3 and original == normalized(translated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", choices=("json", "markdown"), help="Print coverage instead of the short success message")
    parser.add_argument("--require-complete", action="append", metavar="LOCALE", help="Limit the full-coverage check to this locale; may be repeated (default: every bundled locale)")
    args = parser.parse_args()
    template = read_catalog(QT / "locale/bitcoin_en.ts")
    flag_pattern = command_flag_pattern({flag for key in template for flag in COMMAND_FLAGS.findall(key[1])})
    required = MANDATORY | {key for key in template if key[0] in ("MiningPage", "P2CCreateDialog", "P2CClaimDialog")}
    errors = []
    if missing_sources := MANDATORY - template.keys():
        errors.append(f"English template is stale: {sorted(missing_sources)!r}")
    reports = []
    resources = ET.parse(QT / "bitcoin_locale.qrc").getroot().findall(".//file")
    locales = {resource.get("alias") for resource in resources}
    if locales != PLURAL_FORMS.keys():
        parser.error("Update the Qt plural-form counts for the changed locale manifest")
    require_complete = set(args.require_complete) if args.require_complete else locales
    if unknown := require_complete - locales:
        parser.error(f"Unknown locales: {sorted(unknown)}")
    for resource in resources:
        locale = resource.get("alias")
        path = QT / Path(resource.text).with_suffix(".ts")
        catalog = read_catalog(path, expected_language=locale.replace("-", "_"))
        def has_translation(key):
            return available(catalog.get(key), PLURAL_FORMS[locale] if key[3] == "yes" else 1, numerus=key[3] == "yes")
        missing = [key for key in template if not has_translation(key)]
        inherited_review = sum(has_translation(key) and catalog[key].get("type") == "unfinished" for key in template)
        same_as_source = sum(has_translation(key) and len(key[1]) > 30 and len(key[1].split()) > 4
                             and key[1] in forms(catalog.get(key)) for key in template) if locale != "en" else 0
        reports.append({"locale": locale, "active": len(template), "translated": len(template) - len(missing), "missing": len(missing), "unfinished_nonempty": inherited_review, "long_source_identical": same_as_source})
        # Also validate inherited translations that are already available. A
        # partial catalog must not hide malformed substitutions or rich text.
        checked = template.keys() if locale in require_complete else required | {key for key in template if has_translation(key)}
        for key in checked:
            translation = catalog.get(key)
            label = f"{path.name}: {key[0]}: {key[1]!r}"
            if not has_translation(key):
                errors.append(f"{label}: missing translation")
                continue
            if key in required and translation.get("type") == "unfinished":
                errors.append(f"{label}: ConnectCoin-specific translation is unfinished")
            for value in forms(translation):
                if locale != "en" and (locale, key[1]) not in SOURCE_IDENTICAL_EXCEPTIONS and untranslated_sentence(key[1], value):
                    errors.append(f"{label}: untranslated English sentence")
                if not placeholders_match(key[0], key[1], value):
                    errors.append(f"{label}: changed placeholders")
                if Counter(MARKUP.findall(key[1])) != Counter(MARKUP.findall(value)):
                    errors.append(f"{label}: changed rich-text markup")
                if not balanced_markup(value):
                    errors.append(f"{label}: unbalanced rich-text markup")
                # A colon in prose ("connectcoin: click-to-pay handler") may
                # move in translation. Freeze explicitly quoted URI examples.
                if QUOTED_CONNECTCOIN_URI.search(key[1]) and Counter(CONNECTCOIN_URIS.findall(key[1])) != Counter(CONNECTCOIN_URIS.findall(value)):
                    errors.append(f"{label}: changed ConnectCoin URI literal")
                if FILE_FILTERS.search(key[1]) and Counter(FILE_FILTERS.findall(key[1])) != Counter(FILE_FILTERS.findall(value)):
                    errors.append(f"{label}: changed file filter")
                source_flags = command_flag_tokens(key[1], flag_pattern)
                # Do not invent options from translated hyphenated prose in
                # messages without source flags (e.g. Cyrillic pay-to-connect).
                if source_flags and source_flags != command_flag_tokens(value, flag_pattern):
                    errors.append(f"{label}: changed command-line flag or literal value")
                if key in required:
                    # Legacy line breaks and mnemonics remain outside this
                    # check: expanding those rules requires a separate audit.
                    if value.count("\n") != key[1].count("\n"):
                        errors.append(f"{label}: changed line breaks")
                    if value.count("&") != key[1].count("&"):
                        errors.append(f"{label}: changed mnemonic count")
                    for token in ("RandomX", "FAST", "GiB", "CPU", "H/s", "P2PK", "bech32m", "SIGHASH_DEFAULT", "PAY_TO_CONNECT", "-testnet4", "-regtest", "-rpcmaxconnections", "URL"):
                        if key[1].count(token) != value.count(token):
                            errors.append(f"{label}: changed technical token {token}")
    if args.report == "json":
        print(json.dumps(reports, ensure_ascii=False, indent=2))
    elif args.report == "markdown":
        print("| Locale | Nonempty / active | Missing | Unfinished, nonempty | Long English-identical entries |")
        print("| --- | ---: | ---: | ---: | ---: |")
        for row in sorted(reports, key=lambda row: row["locale"]):
            print(f"| `{row['locale']}` | {row['translated']} / {row['active']} | {row['missing']} | {row['unfinished_nonempty']} | {row['long_source_identical']} |")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    if not args.report:
        scope = "all bundled locales" if require_complete == locales else ', '.join(sorted(require_complete))
        print(f"ConnectCoin translations: {len(reports)} catalogs; {len(template)} active messages; full active coverage in {scope}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
