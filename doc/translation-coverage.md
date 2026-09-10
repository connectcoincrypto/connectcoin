# Translation coverage

Snapshot: 2026-09-10. Generated from the current English source catalog and all
100 bundled locale catalogs (including English).

There are **1376 active context/source entries**. All bundled catalogs have a nonempty translation for every active entry, with the expected Qt plural structure. There are **0 missing locale/message pairs**.

This table measures each catalog independently. Regional locales can use their
base-language catalog before falling back to English, but neither fallback
counts toward this coverage. This includes all extracted interface messages;
protocol values, RPC identifiers and intentionally untranslated technical logs
are not natural-language interface strings to rename.

## Reading the table

- **Nonempty** counts entries with text and the expected number and structure
  of Qt plural forms. It measures availability, not linguistic correctness.
- **Unfinished, nonempty** preserves inherited review flags. The build includes
  these translations because `lrelease` does not use `-nounfinished`; removing
  the flag would not establish that someone reviewed the wording.
- **Long English-identical entries** flags non-English entries with a long
  sentence (more than 30 characters and four words) identical to its English
  source in at least one form. There are 0 such flags in this snapshot.
  This is a review aid, not language detection: short residual English and
  partially untranslated sentences may still exist.

The additions are AI-assisted and have automated formatting checks, not
native-speaker certification. Review security warnings and low-resource locales
particularly carefully before claiming production-quality localization.
Linguistic uncertainty is higher in, among others, `am`, `br`, `fo`, `hak`,
`kk@latin`, `mi`, `pam`, `sm`, `szl`, `ve`, and `yo`.

Reproduce the table with:

```sh
python test/lint/lint-connectcoin-translations.py --report markdown
```

See the [translation workflow](translation_process.md) for extraction, validation,
and compiled-resource testing.

## Per-locale availability

| Locale | Nonempty / active | Missing | Unfinished, nonempty | Long English-identical entries |
| --- | ---: | ---: | ---: | ---: |
| `am` | 1376 / 1376 | 0 | 184 | 0 |
| `ar` | 1376 / 1376 | 0 | 951 | 0 |
| `ast_ES` | 1376 / 1376 | 0 | 188 | 0 |
| `az` | 1376 / 1376 | 0 | 352 | 0 |
| `az@latin` | 1376 / 1376 | 0 | 352 | 0 |
| `be` | 1376 / 1376 | 0 | 223 | 0 |
| `bg` | 1376 / 1376 | 0 | 688 | 0 |
| `bn` | 1376 / 1376 | 0 | 182 | 0 |
| `br` | 1376 / 1376 | 0 | 185 | 0 |
| `bs` | 1376 / 1376 | 0 | 358 | 0 |
| `ca` | 1376 / 1376 | 0 | 992 | 0 |
| `cmn` | 1376 / 1376 | 0 | 1125 | 0 |
| `cs` | 1376 / 1376 | 0 | 1147 | 0 |
| `cy` | 1376 / 1376 | 0 | 404 | 0 |
| `da` | 1376 / 1376 | 0 | 943 | 0 |
| `de` | 1376 / 1376 | 0 | 1135 | 0 |
| `el` | 1376 / 1376 | 0 | 1047 | 0 |
| `en` | 1376 / 1376 | 0 | 0 | 0 |
| `eo` | 1376 / 1376 | 0 | 478 | 0 |
| `es` | 1376 / 1376 | 0 | 1146 | 0 |
| `et` | 1376 / 1376 | 0 | 420 | 0 |
| `eu` | 1376 / 1376 | 0 | 1172 | 0 |
| `fa` | 1376 / 1376 | 0 | 893 | 0 |
| `fi` | 1376 / 1376 | 0 | 1038 | 0 |
| `fil` | 1376 / 1376 | 0 | 661 | 0 |
| `fo` | 1376 / 1376 | 0 | 1180 | 0 |
| `fr` | 1376 / 1376 | 0 | 1165 | 0 |
| `ga` | 1376 / 1376 | 0 | 1171 | 0 |
| `ga_IE` | 1376 / 1376 | 0 | 1174 | 0 |
| `gl` | 1376 / 1376 | 0 | 468 | 0 |
| `gl_ES` | 1376 / 1376 | 0 | 464 | 0 |
| `gu` | 1376 / 1376 | 0 | 687 | 0 |
| `hak` | 1376 / 1376 | 0 | 1089 | 0 |
| `he` | 1376 / 1376 | 0 | 825 | 0 |
| `hi` | 1376 / 1376 | 0 | 614 | 0 |
| `hr` | 1376 / 1376 | 0 | 949 | 0 |
| `hu` | 1376 / 1376 | 0 | 1155 | 0 |
| `id` | 1376 / 1376 | 0 | 1011 | 0 |
| `is` | 1376 / 1376 | 0 | 259 | 0 |
| `it` | 1376 / 1376 | 0 | 1110 | 0 |
| `ja` | 1376 / 1376 | 0 | 1169 | 0 |
| `ka` | 1376 / 1376 | 0 | 660 | 0 |
| `kk` | 1376 / 1376 | 0 | 172 | 0 |
| `kk@latin` | 1376 / 1376 | 0 | 1 | 0 |
| `km` | 1376 / 1376 | 0 | 712 | 0 |
| `kn` | 1376 / 1376 | 0 | 157 | 0 |
| `ko` | 1376 / 1376 | 0 | 1147 | 0 |
| `ku` | 1376 / 1376 | 0 | 45 | 0 |
| `ku_IQ` | 1376 / 1376 | 0 | 148 | 0 |
| `lt` | 1376 / 1376 | 0 | 650 | 0 |
| `lv` | 1376 / 1376 | 0 | 313 | 0 |
| `mi` | 1376 / 1376 | 0 | 122 | 0 |
| `mk` | 1376 / 1376 | 0 | 277 | 0 |
| `ml` | 1376 / 1376 | 0 | 338 | 0 |
| `mn` | 1376 / 1376 | 0 | 224 | 0 |
| `ms` | 1376 / 1376 | 0 | 143 | 0 |
| `mt` | 1376 / 1376 | 0 | 477 | 0 |
| `nb` | 1376 / 1376 | 0 | 913 | 0 |
| `ne` | 1376 / 1376 | 0 | 207 | 0 |
| `nl` | 1376 / 1376 | 0 | 1052 | 0 |
| `no` | 1376 / 1376 | 0 | 917 | 0 |
| `pam` | 1376 / 1376 | 0 | 211 | 0 |
| `pl` | 1376 / 1376 | 0 | 1167 | 0 |
| `ps` | 1376 / 1376 | 0 | 399 | 0 |
| `pt` | 1376 / 1376 | 0 | 1137 | 0 |
| `pt_BR` | 1376 / 1376 | 0 | 1052 | 0 |
| `ro` | 1376 / 1376 | 0 | 863 | 0 |
| `ru` | 1376 / 1376 | 0 | 1135 | 0 |
| `si` | 1376 / 1376 | 0 | 274 | 0 |
| `sk` | 1376 / 1376 | 0 | 1142 | 0 |
| `sl` | 1376 / 1376 | 0 | 1035 | 0 |
| `sm` | 1376 / 1376 | 0 | 447 | 0 |
| `sq` | 1376 / 1376 | 0 | 154 | 0 |
| `sr` | 1376 / 1376 | 0 | 852 | 0 |
| `sr@ijekavianlatin` | 1376 / 1376 | 0 | 111 | 0 |
| `sr@latin` | 1376 / 1376 | 0 | 199 | 0 |
| `sv` | 1376 / 1376 | 0 | 898 | 0 |
| `sw` | 1376 / 1376 | 0 | 327 | 0 |
| `szl` | 1376 / 1376 | 0 | 357 | 0 |
| `ta` | 1376 / 1376 | 0 | 692 | 0 |
| `te` | 1376 / 1376 | 0 | 591 | 0 |
| `th` | 1376 / 1376 | 0 | 849 | 0 |
| `tk` | 1376 / 1376 | 0 | 568 | 0 |
| `tl` | 1376 / 1376 | 0 | 600 | 0 |
| `tr` | 1376 / 1376 | 0 | 963 | 0 |
| `uk` | 1376 / 1376 | 0 | 1192 | 0 |
| `ur` | 1376 / 1376 | 0 | 607 | 0 |
| `uz` | 1376 / 1376 | 0 | 613 | 0 |
| `uz@Cyrl` | 1376 / 1376 | 0 | 616 | 0 |
| `uz@Latn` | 1376 / 1376 | 0 | 619 | 0 |
| `ve` | 1376 / 1376 | 0 | 134 | 0 |
| `yi` | 1376 / 1376 | 0 | 230 | 0 |
| `yo` | 1376 / 1376 | 0 | 123 | 0 |
| `yue` | 1376 / 1376 | 0 | 1080 | 0 |
| `zh` | 1376 / 1376 | 0 | 1101 | 0 |
| `zh-Hans` | 1376 / 1376 | 0 | 1134 | 0 |
| `zh-Hant` | 1376 / 1376 | 0 | 1121 | 0 |
| `zh_CN` | 1376 / 1376 | 0 | 1111 | 0 |
| `zh_HK` | 1376 / 1376 | 0 | 1120 | 0 |
| `zh_TW` | 1376 / 1376 | 0 | 1104 | 0 |
