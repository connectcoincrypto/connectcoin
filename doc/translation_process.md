# Translations

ConnectCoin currently inherits Bitcoin Core's Qt translation catalogs, file
names, resource aliases, and extraction tooling. Those catalogs have not been
reviewed as ConnectCoin translations and must not be presented as project-owned
or production-ready localization.

Changed ConnectCoin source strings fall back to English unless a matching,
reviewed translation is added. The upstream Bitcoin Transifex project and
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

## Adding a language

Until that workflow exists, new translations require explicit maintainer review.
The language must be added to the inherited Qt resource manifest, and the
compiled resource must end in `.qm`. Never copy Bitcoin-branded translated
strings into a ConnectCoin release without reviewing product names, URI schemes,
units, executable names, data paths, and security wording.
