# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Hermetic checks for ccache diagnostics; no cache, compiler or network needed."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "ci/test/ccache_stats.py"
SPEC = importlib.util.spec_from_file_location("ccache_stats", HELPER)
ccache_stats = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ccache_stats)


def counters(**updates):
    values = dict.fromkeys(ccache_stats.CALL_COUNTERS | ccache_stats.ERROR_COUNTERS | ccache_stats.UNCACHEABLE_COUNTERS, 0)
    values.update(updates)
    return values


class CcacheStatisticsTests(unittest.TestCase):
    def test_actual_asan_counters_expose_hidden_fallbacks(self):
        result = ccache_stats.summarize(counters(direct_cache_hit=367, preprocessed_cache_hit=105, cache_miss=30, bad_input_file=347))
        self.assertIn("cacheable calls): 94.02% (472/502)", result)
        self.assertIn("all compiler calls): 55.59% (472/849)", result)
        self.assertIn("bad_input_file=347", result)
        self.assertIn("::notice title=ccache fallbacks::", result)

    def test_actual_no_wallet_counters(self):
        result = ccache_stats.summarize(counters(direct_cache_hit=647, cache_miss=22, bad_input_file=281))
        self.assertIn("cacheable calls): 96.71% (647/669)", result)
        self.assertIn("all compiler calls): 68.11% (647/950)", result)

    def test_all_error_and_uncacheable_categories_count_once(self):
        values = counters(direct_cache_hit=100, cache_miss=20)
        values.update(dict.fromkeys(ccache_stats.ERROR_COUNTERS | ccache_stats.UNCACHEABLE_COUNTERS | ccache_stats.OPTIONAL_OUTCOMES, 1))
        total = sum(values.values())
        result = ccache_stats.summarize(values)
        self.assertIn(f"(100/{total})", result)

    def test_storage_and_metadata_do_not_inflate_calls(self):
        values = counters(direct_cache_hit=75, preprocessed_cache_hit=5, cache_miss=20)
        values.update(dict.fromkeys(ccache_stats.NON_OUTCOME_COUNTERS, 100000))
        result = ccache_stats.summarize(values)
        self.assertIn("all compiler calls): 80.00% (80/100)", result)
        self.assertNotIn("storage lookups only", result)

    def test_newer_optional_counter(self):
        result = ccache_stats.summarize(counters(direct_cache_hit=80, unsupported_source_encoding=20))
        self.assertIn("uncacheable=20", result)
        self.assertIn("all compiler calls): 80.00% (80/100)", result)

    def test_legacy_storage_only(self):
        for prefix in ("local_storage", "primary_storage"):
            with self.subTest(prefix=prefix):
                result = ccache_stats.summarize({f"{prefix}_hit": 4, f"{prefix}_miss": 6, "bad_input_file": 9})
                self.assertIn("storage lookups only): 40.00% (4/10)", result)
                self.assertIn("effective hit rate: unavailable", result)
                self.assertIn("bad_input_file=9", result)

    def test_local_storage_pair_preferred_over_primary(self):
        result = ccache_stats.summarize({"local_storage_hit": 9, "local_storage_miss": 1, "primary_storage_hit": 1, "primary_storage_miss": 9})
        self.assertIn("storage lookups only): 90.00% (9/10)", result)

    def test_incomplete_pair_does_not_mix_versions(self):
        result = ccache_stats.summarize({"local_storage_hit": 9, "primary_storage_miss": 1})
        self.assertIn("hit rate: unavailable", result)

    def test_zero_calls_no_notice_or_division(self):
        result = ccache_stats.summarize(counters())
        self.assertIn("effective hit rate: no calls", result)
        self.assertNotIn("::notice", result)

    def test_only_errors_still_reported(self):
        result = ccache_stats.summarize(counters(bad_input_file=10))
        self.assertIn("all compiler calls): 0.00% (0/10)", result)
        self.assertIn("bad_input_file=10", result)

    def test_missing_outcome_counter_disables_effective_rate(self):
        values = counters(direct_cache_hit=9, cache_miss=1)
        del values["internal_error"]
        result = ccache_stats.summarize(values)
        self.assertIn("cacheable calls): 90.00%", result)
        self.assertIn("effective hit rate: unavailable", result)

    def test_unknown_nonzero_counter_disables_effective_rate(self):
        result = ccache_stats.summarize(counters(direct_cache_hit=9, future_outcome=1))
        self.assertIn("effective hit rate: unavailable", result)

    def test_tab_format_and_crlf(self):
        stats, invalid = ccache_stats.parse_stats(["cache_miss\t7\r\n", "\n", "direct_cache_hit\t2\n"])
        self.assertEqual(stats, {"cache_miss": 7, "direct_cache_hit": 2})
        self.assertFalse(invalid)

    def test_malformed_negative_overflow_duplicate_rows(self):
        for row in ("broken", "cache_miss\t-1", "cache_miss\tnan", "cache_miss\t1.5", "cache_miss\t18446744073709551616", "cache_miss\t1\textra", "\t1", "direct_cache_hit\t3"):
            with self.subTest(row=row):
                stats, invalid = ccache_stats.parse_stats(["direct_cache_hit\t2", row])
                self.assertTrue(invalid)
                self.assertEqual(stats, {"direct_cache_hit": 2})
                self.assertIn("effective hit rate: unavailable", ccache_stats.summarize(stats, invalid=invalid))

    def test_annotation_escapes_container_without_new_commands(self):
        result = ccache_stats.summarize(counters(bad_input_file=1), container="name%\r\n::error::injected")
        self.assertIn("name%25%0D%0A::error::injected", result)
        self.assertNotIn("\n::error::", result)

    def test_empty_cli_without_environment_is_successful(self):
        env = dict(os.environ)
        env.pop("CONTAINER_NAME", None)
        result = subprocess.run([sys.executable, str(HELPER)], input="", text=True, capture_output=True, env=env, timeout=10, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hit rate: unavailable", result.stdout)

    def test_ci_script_invokes_helper(self):
        source = (ROOT / "ci/test/03_test_script.sh").read_text(encoding="utf-8")
        self.assertIn('ccache --print-stats | python3 "${BASE_ROOT_DIR}/ci/test/ccache_stats.py"', source)


if __name__ == "__main__":
    unittest.main()
