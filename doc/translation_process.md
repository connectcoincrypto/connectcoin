# Translations

ConnectCoin currently inherits Bitcoin Core's Qt translation catalogs, file
names, resource aliases, and extraction tooling. Those catalogs have not been
reviewed as ConnectCoin translations and must not be presented as project-owned
or production-ready localization.

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

For ordinary feature changes, avoid committing mechanically regenerated
catalogs. A project-owned translation workflow, reviewer policy, locale
manifest, and publication channel are pre-release requirements.

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

Until that workflow exists, new translations require explicit maintainer review.
The language must be added to the inherited Qt resource manifest, and the
compiled resource must end in `.qm`. Never copy Bitcoin-branded translated
strings into a ConnectCoin release without reviewing product names, URI schemes,
units, executable names, data paths, and security wording.
