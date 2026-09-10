# Translations

ConnectCoin maintains Qt translation catalogs inherited from Bitcoin Core,
including their file names, resource aliases, and extraction tooling. New
ConnectCoin translations are AI-assisted; they must not be represented as
native-speaker-reviewed localization. The [coverage report](translation-coverage.md)
records availability separately from the inherited `unfinished` review flags.

Changed ConnectCoin source strings fall back to English unless a matching
translation is added. The upstream Bitcoin Transifex project and
Bitcoin translation mailing list are not ConnectCoin support channels.
The inherited `.tx/config` was removed so routine tooling cannot accidentally
pull from or push ConnectCoin strings to Bitcoin Core's Transifex project.

## Updating source strings

The inherited source files remain named `bitcoin_xx_YY.ts` or `bitcoin_xx.ts`
for build-tool compatibility. `src/qt/locale/bitcoin_en.ts` is the extraction
source for the other catalogs. Do not rename these files independently; a
future rename must update the Qt resources, CMake targets, tooling, and all
catalog references together.

Regenerate extracted strings with:

```sh
cmake --preset dev-mode -DWITH_USDT=OFF -DENABLE_IPC=OFF
cmake --build build_dev_mode --target translate
```

The `translate` target requires GNU gettext (`xgettext`) and Qt Linguist
(`lupdate`). It regenerates the backend string definitions as well as the
English catalog. Source lists are passed through files, so extraction also
works on Windows without exceeding its command-line length limit.

For ordinary feature changes, update the affected translations alongside the
source. Match entries by context, exact case-sensitive source, disambiguation
comment, and plural status. Preserve existing reviewed wording and use the
English source only for the English catalog, never as a substitute for a
missing translation in another locale. Do not clear inherited `unfinished`
flags wholesale: our `lrelease` build includes their nonempty translations,
and changing a flag is not a linguistic review.

After updating catalogs, run:

```sh
python test/lint/lint-p2c-translations.py
python test/lint/lint-connectcoin-translations.py
python test/lint/lint-test-connectcoin-translations.py
python test/lint/lint-connectcoin-translations.py --report markdown
```

The second check requires all active messages in every bundled locale by default,
including inherited dialogs and backend diagnostics, not only P2C and mining.
For an intermediate audit, limit the full-coverage check with
`--require-complete LOCALE` (repeat the option for multiple locales). The report
counts only existing, nonempty translations with the expected Qt plural forms,
not English fallbacks. The plural table records the actual Qt language tags,
including script variants; recheck it when changing these tags or Qt's rules.
Each catalog is counted independently; the runtime can consult a regional
locale's base-language catalog before falling back to English. The report
does not certify translation quality. Update
the checked-in coverage snapshot after changing the source catalog or translations.

Build the wallet and run the Qt wallet tests as well: these load the actual
compiled `.qm` resources, query every active message in every bundled catalog,
exercise plural branches without base-language or English fallback, and check
rendered Portuguese labels. The source catalog is embedded in the test binary
only, so the test also works outside the source checkout. The lint rejects
copied English sentences, including short storage plurals; legitimate identical
labels and protocol identifiers are not translations to invent. These checks
do not establish linguistic correctness or native-speaker review.
The lint also checks Qt/printf substitutions, rich-text tags and their nesting,
and command-line option names and literal values. Line-break and mnemonic-count
checks are strict for the ConnectCoin-specific messages; they do not certify
every inherited shortcut or text layout.
Both translation lints and their regression tests use the `lint-*.py` naming
convention so the repository's CI lint runner discovers them automatically.

## P2C interface translations

The P2C creation and automatic-claim pages use the existing locale manifest.
Their translations include the confirmation and privacy warnings, progress
states, navigation text, and P2C labels in transaction history. RPC state
identifiers and technical diagnostics remain untranslated; only their GUI
presentation is localized. Do not translate RPC values to fix UI text.

These are initial AI-assisted translations, not native-speaker-approved
localization. Formatting checks do not establish linguistic correctness.
Native speakers should review terminology and, especially, the warnings about
direct connections, IP visibility, fees, and irreversible bounty funding before
release. The presence of P2C translations does not certify the inherited
translations elsewhere in the application.

After changing a P2C source string, update its corresponding entries in the
bundled catalogs and run:

```sh
python test/lint/lint-p2c-translations.py
```

This check verifies coverage, placeholders, markup, accelerators, line breaks,
and stable technical identifiers. The Qt wallet tests also load the compiled
translation resources and check the rendered Portuguese page labels. Keep
catalog updates scoped to the relevant messages instead of rewriting unrelated
inherited translations.

## Adding a language

New languages require explicit maintainer review.
The language must be added to the inherited Qt resource manifest, and the
compiled resource must end in `.qm`. Never copy Bitcoin-branded translated
strings into a ConnectCoin release without reviewing product names, URI schemes,
units, executable names, data paths, and security wording.
