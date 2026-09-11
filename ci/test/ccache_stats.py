# Copyright (c) 2026 The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Summarize ccache's tab-separated --print-stats without hiding fallbacks."""

import os
import sys

# Outcome counters from core/Statistics.cpp in ccache 4.9.1 through 4.14.
# Storage accesses, intermediate lookup misses, sizes and timestamps are not
# compiler-call outcomes. Keep those out of the effective hit-rate denominator.
ERROR_COUNTERS = frozenset({
    "bad_input_file", "bad_output_file", "compiler_check_failed",
    "could_not_find_compiler", "error_hashing_extra_file", "internal_error",
    "missing_cache_file", "modified_input_file",
})
UNCACHEABLE_COUNTERS = frozenset({
    "autoconf_test", "bad_compiler_arguments", "called_for_link",
    "called_for_preprocessing", "compile_failed", "compiler_produced_no_output",
    "compiler_produced_empty_output", "compiler_produced_stdout",
    "could_not_use_modules", "could_not_use_precompiled_header", "disabled",
    "multiple_source_files", "no_input_file", "output_to_stdout",
    "preprocessor_error", "recache", "unsupported_code_directive",
    "unsupported_compiler_option", "unsupported_environment_variable",
    "unsupported_source_language",
})
# Added after 4.9.1; absent in earlier versions rather than an incomplete dump.
OPTIONAL_OUTCOMES = frozenset({"unsupported_source_encoding"})
CALL_COUNTERS = frozenset({"direct_cache_hit", "preprocessed_cache_hit", "cache_miss"})
NON_OUTCOME_COUNTERS = frozenset({
    "cache_size_kibibyte", "cleanups_performed", "files_in_cache",
    "max_cache_size_kibibyte", "max_files_in_cache", "stats_updated_timestamp",
    "stats_zeroed_timestamp", "direct_cache_miss", "preprocessed_cache_miss",
    "local_storage_hit", "local_storage_miss", "local_storage_read_hit",
    "local_storage_read_miss", "local_storage_write", "primary_storage_hit",
    "primary_storage_miss", "primary_storage_read_hit", "primary_storage_read_miss",
    "primary_storage_write", "remote_storage_error", "remote_storage_hit",
    "remote_storage_miss", "remote_storage_read_hit", "remote_storage_read_miss",
    "remote_storage_write", "remote_storage_timeout",
})


def parse_stats(lines):
    """Ignore malformed rows without echoing their potentially arbitrary text."""
    stats = {}
    invalid = False
    for line in lines:
        if not line.strip():
            continue
        fields = line.rstrip("\r\n").split("\t")
        if len(fields) != 2:
            invalid = True
            continue
        key, raw = fields
        try:
            value = int(raw)
        except ValueError:
            invalid = True
            continue
        if not key or not 0 <= value <= (1 << 64) - 1 or key in stats:
            invalid = True
            continue
        stats[key] = value
    return stats, invalid


def escape_annotation(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def summarize(stats, *, invalid=False, container="unknown"):
    lines = []

    def notice(title, message):
        lines.append(f"::notice title={title}::{escape_annotation(message)}")

    hits = misses = None
    if CALL_COUNTERS <= stats.keys():
        hits = stats["direct_cache_hit"] + stats["preprocessed_cache_hit"]
        misses = stats["cache_miss"]
        rate_label = "cacheable calls"
    else:
        # Older/partial formats can still expose storage statistics. Do not
        # mistake them for compiler calls or use them in an effective rate.
        for prefix in ("local_storage", "primary_storage"):
            if {f"{prefix}_hit", f"{prefix}_miss"} <= stats.keys():
                hits, misses = stats[f"{prefix}_hit"], stats[f"{prefix}_miss"]
                break
        rate_label = "storage lookups only"

    if hits is not None and misses is not None:
        calls = hits + misses
        if calls:
            rate = 100 * hits / calls
            lines.append(f"Ccache hit rate ({rate_label}): {rate:.2f}% ({hits}/{calls})")
            if rate < 75:
                notice("low ccache hitrate", f"Ccache hit rate ({rate_label}) in {container} was {rate:.2f}%")
        else:
            lines.append(f"Ccache hit rate ({rate_label}): no calls")
    else:
        lines.append("Ccache hit rate: unavailable (missing call/storage counters)")

    errors = sum(stats.get(key, 0) for key in ERROR_COUNTERS)
    uncacheable = sum(stats.get(key, 0) for key in UNCACHEABLE_COUNTERS | OPTIONAL_OUTCOMES)
    bad_input = stats.get("bad_input_file", "unavailable")
    lines.append(f"Ccache reported fallbacks: errors={errors}, uncacheable={uncacheable}, bad_input_file={bad_input}")
    required = CALL_COUNTERS | ERROR_COUNTERS | UNCACHEABLE_COUNTERS
    known = required | OPTIONAL_OUTCOMES | NON_OUTCOME_COUNTERS
    unknown_outcomes = any(value for key, value in stats.items() if key not in known)
    if required <= stats.keys() and not invalid and not unknown_outcomes:
        total = hits + misses + errors + uncacheable
        if total:
            lines.append(f"Ccache effective hit rate (all compiler calls): {100 * hits / total:.2f}% ({hits}/{total})")
        else:
            lines.append("Ccache effective hit rate: no calls")
    else:
        lines.append("Ccache effective hit rate: unavailable (incomplete or unrecognized counters)")
    if errors or uncacheable:
        notice("ccache fallbacks", f"Ccache in {container} reported {errors} errors and {uncacheable} uncacheable calls; bad_input_file={bad_input}. These are excluded from the cacheable hit rate.")
    if invalid:
        notice("ccache statistics", "Some malformed or duplicate statistics were ignored; no effective hit rate was calculated.")
    return "\n".join(lines)


def main():
    stats, invalid = parse_stats(sys.stdin)
    print(summarize(stats, invalid=invalid, container=os.environ.get("CONTAINER_NAME", "unknown")))


if __name__ == "__main__":
    main()
