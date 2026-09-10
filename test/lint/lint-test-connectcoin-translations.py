# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Regression tests for translation coverage and placeholder checks."""

import importlib.util
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("catalog_lint", Path(__file__).with_name("lint-connectcoin-translations.py"))
assert SPEC is not None and SPEC.loader is not None
CATALOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CATALOG)


class TranslationCatalogTests(unittest.TestCase):
    def run_main_with_translation(self, value, omit=False, *, source=None, context_name='bitcoin-core', extra_sources=()):
        if source is None:
            source = 'Unable to replay blocks. You will need to rebuild the database using -reindex-chainstate.'
        with tempfile.TemporaryDirectory(prefix='connectcoin-i18n-main-') as directory:
            qt = Path(directory)
            (qt / 'locale').mkdir()
            for locale in ('en', 'pt'):
                root = ET.Element('TS', language=locale)
                context = ET.SubElement(root, 'context')
                ET.SubElement(context, 'name').text = context_name
                if locale == 'en' or not omit:
                    message = ET.SubElement(context, 'message')
                    ET.SubElement(message, 'source').text = source
                    ET.SubElement(message, 'translation').text = source if locale == 'en' else value
                for extra_source in extra_sources:
                    message = ET.SubElement(context, 'message')
                    ET.SubElement(message, 'source').text = extra_source
                    ET.SubElement(message, 'translation').text = extra_source
                ET.ElementTree(root).write(qt / f'locale/bitcoin_{locale}.ts', encoding='utf-8')
            (qt / 'bitcoin_locale.qrc').write_text('<RCC><qresource>'
                '<file alias="en">locale/bitcoin_en.qm</file>'
                '<file alias="pt">locale/bitcoin_pt.qm</file>'
                '</qresource></RCC>', encoding='utf-8')
            with patch.object(CATALOG, 'QT', qt), patch.object(CATALOG, 'PLURAL_FORMS', {'en': 2, 'pt': 2}), \
                    patch.object(CATALOG, 'MANDATORY', set()), patch.object(sys, 'argv', ['translation-lint']), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
                return CATALOG.main(), errors.getvalue()

    def test_main_requires_all_catalogs_and_preserves_flags_and_html(self):
        valid = 'Não foi possível repetir os blocos. Reconstrua o banco de dados usando -reindex-chainstate.'
        self.assertEqual(self.run_main_with_translation(valid), (0, ''))
        self.assertEqual(self.run_main_with_translation('使用-reindex-chainstate。'), (0, ''))
        self.assertEqual(self.run_main_with_translation('Die -reindex-chainstate-Option verwenden.'), (0, ''))
        code, errors = self.run_main_with_translation(valid, omit=True)
        self.assertEqual(code, 1)
        self.assertIn('missing translation', errors)
        code, errors = self.run_main_with_translation(valid.replace('-reindex-chainstate', ''))
        self.assertEqual(code, 1)
        self.assertIn('changed command-line flag', errors)
        code, errors = self.run_main_with_translation('<a href="#unexpected">' + valid + '</a>')
        self.assertEqual(code, 1)
        self.assertIn('changed rich-text markup', errors)

    def test_complete_flag_names_and_linguistic_suffixes(self):
        pattern = CATALOG.command_flag_pattern({'-reindex', '-reindex-chainstate', '-proxy'})
        def tokens(text):
            return CATALOG.command_flag_tokens(text, pattern)
        self.assertNotEqual(tokens('-reindex'), tokens('-reindex-chainstate'))
        self.assertNotEqual(tokens('-reindex-chainstate'), tokens('-reindex'))
        self.assertNotEqual(tokens('-reindex'), tokens('--reindex'))
        self.assertNotEqual(tokens('-reindex'), tokens('-reindexing'))
        self.assertNotEqual(tokens('-reindex'), tokens('-reindex-chainstateTYPO'))
        self.assertEqual(tokens('-reindex-chainstate'), tokens('Die -reindex-chainstate-Option'))
        self.assertEqual(tokens('-reindex'), tokens('käytä -reindex-asetusta'))
        self.assertEqual(tokens('-proxy'), tokens('l-proxy: -proxy'))
        self.assertEqual(tokens('-proxy'), tokens('使用-proxy。'))
        self.assertEqual(tokens('-proxy'), tokens('השתמשו ב-proxy'))
        code, errors = self.run_main_with_translation('Reinicie com -reindex-chainstate.',
            source='Please restart with -reindex.', extra_sources=('-reindex-chainstate',))
        self.assertEqual(code, 1)
        self.assertIn('changed command-line flag', errors)
        self.assertEqual(self.run_main_with_translation('Die -reindex-Option verwenden.',
            source='Please restart with -reindex.', extra_sources=('-reindex-chainstate',)), (0, ''))
        self.assertEqual(self.run_main_with_translation('Пай-то-connect мукофотларини яратиш',
            source='Create pay-to-connect bounties', context_name='BitcoinGUI', extra_sources=('-connect',)), (0, ''))

    def test_literal_flag_values_but_not_metavariables(self):
        pattern = CATALOG.command_flag_pattern({'-onlynet', '-onion', '-bind', '-dumpfile', '-proxy'})
        def tokens(text):
            return CATALOG.command_flag_tokens(text, pattern)
        self.assertNotEqual(tokens('-onlynet=cjdns'), tokens('-onlynet=i2p'))
        self.assertNotEqual(tokens('-onion=0'), tokens('-onion=1'))
        self.assertNotEqual(tokens('-onion=0'), tokens('-onion=0.1'))
        self.assertNotEqual(tokens('-onion=0'), tokens('-onion'))
        self.assertEqual(tokens('-onion=0'), tokens('-onion=0.'))
        self.assertEqual(tokens('-onlynet=onion'), tokens('-onlynet="onion"'))
        self.assertEqual(tokens('-dumpfile=<filename>'), tokens('-dumpfile=<nome do arquivo>'))
        self.assertEqual(tokens('-dumpfile="<filename>"'), tokens('-dumpfile="<nom de fichier>"'))
        self.assertEqual(tokens('-bind=...=onion'), tokens('-bind=…=onion'))
        self.assertEqual(tokens('-bind=...=onion'), tokens('-bind=...=onion- und andere Einstellungen'))
        self.assertEqual(tokens('-onion=0'), tokens('-onion=0-ийг'))
        self.assertNotEqual(tokens('-bind=...=onion'), tokens('-bind=...=i2p'))
        self.assertEqual(tokens("-proxy='%s'"), tokens('-proxy=%s'))
        self.assertNotEqual(tokens('-onlynet=cjdns -onlynet=i2p'), tokens('-onlynet=i2p -onlynet=i2p'))
        self.assertEqual(self.run_main_with_translation('Forneça -dumpfile=<nome do arquivo>.',
            source='Provide -dumpfile=<filename>.'), (0, ''))
        code, errors = self.run_main_with_translation('Não use -onion=1.', source='Do not use -onion=0.')
        self.assertEqual(code, 1)
        self.assertIn('changed command-line flag or literal value', errors)

    def test_qt_markup_is_balanced_without_freezing_sentence_order(self):
        self.assertTrue(CATALOG.balanced_markup('<b>Bold <i>italic</i></b><br><hr/><img src="icon.png">'))
        self.assertTrue(CATALOG.balanced_markup('<amount> <wallet name> <filename>'))
        self.assertFalse(CATALOG.balanced_markup('</b>Bold<b>'))
        self.assertFalse(CATALOG.balanced_markup('<b><i>Bold italic</b></i>'))
        self.assertFalse(CATALOG.balanced_markup('<b>Unclosed'))
        code, errors = self.run_main_with_translation('Proxy está </b>ativado<b>: %1',
            source='Proxy is <b>enabled</b>: %1', context_name='BitcoinGUI')
        self.assertEqual(code, 1)
        self.assertIn('unbalanced rich-text markup', errors)
        self.assertEqual(self.run_main_with_translation('<i>segundo</i> e <b>primeiro</b>',
            source='<b>first</b> and <i>second</i>', context_name='BitcoinGUI'), (0, ''))

    def test_localized_numerus_placeholder_reaches_main(self):
        self.assertTrue(CATALOG.placeholders_match('BitcoinGUI', '%Ln blocks', '%Ln blocos'))
        self.assertFalse(CATALOG.placeholders_match('BitcoinGUI', '%Ln blocks', '%n blocos'))
        self.assertFalse(CATALOG.placeholders_match('BitcoinGUI', '%Ln blocks', 'blocos'))
        code, errors = self.run_main_with_translation('blocos', source='%Ln blocks', context_name='BitcoinGUI')
        self.assertEqual(code, 1)
        self.assertIn('changed placeholders', errors)

    def test_available_unfinished_is_not_english_fallback(self):
        self.assertTrue(CATALOG.available(ET.fromstring('<translation type="unfinished">Carteira</translation>')))
        self.assertFalse(CATALOG.available(None))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation type="unfinished"></translation>')))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation type="vanished">Carteira</translation>')))

    def test_all_plural_forms_must_exist(self):
        self.assertTrue(CATALOG.available(ET.fromstring('<translation><numerusform>%n bloco</numerusform><numerusform>%n blocos</numerusform></translation>')))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation><numerusform>%n bloco</numerusform><numerusform></numerusform></translation>')))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation><numerusform>%n bloco</numerusform></translation>'), 2))
        self.assertTrue(CATALOG.available(ET.fromstring('<translation><numerusform>%n bloco</numerusform><numerusform>%n blocos</numerusform></translation>'), 2))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation>%n 件</translation>'), 1, numerus=True))
        self.assertTrue(CATALOG.available(ET.fromstring('<translation><numerusform>%n 件</numerusform></translation>'), 1, numerus=True))
        self.assertFalse(CATALOG.available(ET.fromstring('<translation><numerusform>Texto</numerusform></translation>'), 1, numerus=False))

    def test_placeholder_grammars(self):
        self.assertEqual(CATALOG.QT_PLACEHOLDERS.findall('%7ATENÇÃO%8 %L1 %n %10 %Ln'), ['%7', '%8', '%L1', '%n', '%10', '%Ln'])
        self.assertEqual(CATALOG.PRINTF_PLACEHOLDERS.findall('%s %zu %d %.2f %%'), ['%s', '%zu', '%d', '%.2f', '%%'])
        self.assertFalse(CATALOG.placeholders_match('bitcoin-core', '%s %d', '%d %s'))
        self.assertTrue(CATALOG.placeholders_match('bitcoin-core', '%1$s %2$d', '%2$d %1$s'))
        self.assertTrue(CATALOG.placeholders_match('RPCConsole', '%7WARNING%8', '%7ATENÇÃO%8'))
        self.assertTrue(CATALOG.placeholders_match('BitcoinGUI', '%1 %2', '%2 %1'))

    def test_copied_english_sentences_are_not_translations(self):
        self.assertTrue(CATALOG.untranslated_sentence('%n GB of space available', '%n GB of space available'))
        self.assertTrue(CATALOG.untranslated_sentence('(of %n GB needed)', '(of %n GB needed)'))
        self.assertTrue(CATALOG.untranslated_sentence('Could not open wallet.', 'Could not open wallet'))
        self.assertFalse(CATALOG.untranslated_sentence('ID', 'ID'))
        self.assertFalse(CATALOG.untranslated_sentence('Send P2C', 'Send P2C'))  # Correct Danish/Norwegian.
        self.assertFalse(CATALOG.untranslated_sentence('Stop HTTPS (0)', 'Stop HTTPS (0)'))  # Also correct Irish/Danish.
        self.assertFalse(CATALOG.untranslated_sentence('%n GB', '%n GB'))
        self.assertFalse(CATALOG.untranslated_sentence('(%n GB needed for full chain)', '（完整区块链需要 %n GB）'))
        self.assertFalse(CATALOG.untranslated_sentence('ConnectCoin supports P2PK addresses', 'ConnectCoin 支持 P2PK 地址'))

    def test_case_sensitive_context_and_disambiguation(self):
        with tempfile.TemporaryDirectory(prefix="connectcoin-i18n-") as directory:
            path = Path(directory) / "catalog.ts"
            path.write_text('<TS><context><name>Wallet</name>'
                            '<message><source>Migrate Wallet</source><translation>A</translation></message>'
                            '<message><source>Migrate wallet</source><translation>B</translation></message>'
                            '<message><source>Migrate wallet</source><comment>verb</comment><translation>C</translation></message>'
                            '</context></TS>', encoding="utf-8")
            self.assertEqual(len(CATALOG.read_catalog(path)), 3)
            with self.assertRaisesRegex(ValueError, "unexpected language tag"):
                CATALOG.read_catalog(path, expected_language="pt_BR")
            path.write_text('<TS><context><name>Wallet</name>'
                            '<message><source>A</source><translation>A</translation></message>'
                            '<message><source>A</source><translation>B</translation></message>'
                            '</context></TS>', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate message"):
                CATALOG.read_catalog(path)


if __name__ == "__main__":
    unittest.main()
