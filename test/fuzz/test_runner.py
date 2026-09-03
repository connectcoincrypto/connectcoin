#!/usr/bin/env python3
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Run fuzz test targets.
"""

from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import argparse
import configparser
import logging
import os
import random
import subprocess
import sys
import tempfile


def get_fuzz_env(*, target, source_dir):
    symbolizer = os.environ.get('LLVM_SYMBOLIZER_PATH', "/usr/bin/llvm-symbolizer")
    fuzz_env = os.environ | {
        'FUZZ': target,
        'UBSAN_OPTIONS':
        f'suppressions={source_dir}/test/sanitizer_suppressions/ubsan:print_stacktrace=1:halt_on_error=1:report_error_type=1',
        'UBSAN_SYMBOLIZER_PATH': symbolizer,
        "ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1",
        'ASAN_SYMBOLIZER_PATH': symbolizer,
        'MSAN_SYMBOLIZER_PATH': symbolizer,
    }
    return fuzz_env


def select_fuzz_shard(*, targets, corpus_dir, shard_count, shard_index):
    """Balance targets by estimated corpus work and return one deterministic shard."""
    weighted_targets = []
    for target in targets:
        target_corpus = corpus_dir / target
        try:
            with os.scandir(target_corpus) as entries:
                corpus_files = [entry for entry in entries if entry.is_file()]
        except FileNotFoundError:
            corpus_files = []

        input_count = len(corpus_files)
        input_bytes = sum(entry.stat(follow_symlinks=False).st_size for entry in corpus_files)
        # Every input has fixed harness/setup cost, while larger inputs also take
        # longer to deserialize and execute. Counting only files put several of
        # the most expensive corpora on the same shard despite equal file-count
        # totals. One extra work unit per 4 KiB is a deterministic approximation
        # that balances both costs. Empty corpora run for --empty_min_time, so
        # they still represent work.
        estimated_work = max(input_count + (input_bytes + 4095) // 4096, 1)
        weighted_targets.append((target, estimated_work))

    shards = [[] for _ in range(shard_count)]
    shard_loads = [0] * shard_count
    for target, estimated_work in sorted(weighted_targets, key=lambda item: (-item[1], item[0])):
        lightest_shard = min(range(shard_count), key=lambda index: (shard_loads[index], index))
        shards[lightest_shard].append(target)
        shard_loads[lightest_shard] += estimated_work

    # Targets were assigned in decreasing estimated-work order. Preserve that
    # order inside each shard so expensive corpora start first and do not create
    # a long serial tail after all smaller targets have completed.
    return shards[shard_index], shard_loads


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description='''Run the fuzz targets with all inputs from the corpus_dir once.''',
    )
    parser.add_argument(
        "-l",
        "--loglevel",
        dest="loglevel",
        default="INFO",
        help="log events at this level and higher to the console. Can be set to DEBUG, INFO, WARNING, ERROR or CRITICAL. Passing --loglevel DEBUG will output all logs to console.",
    )
    parser.add_argument(
        '--valgrind',
        action='store_true',
        help='If true, run fuzzing binaries under the valgrind memory error detector',
    )
    parser.add_argument(
        "--empty_min_time",
        type=int,
        help="If set, run at least this long, if the existing fuzz inputs directory is empty.",
    )
    parser.add_argument(
        '-x',
        '--exclude',
        help="A comma-separated list of targets to exclude",
    )
    parser.add_argument(
        '--par',
        '-j',
        type=int,
        default=4,
        help='How many targets to merge or execute in parallel.',
    )
    parser.add_argument(
        '--shard-count',
        type=int,
        default=1,
        help='Balance the fuzz target list by estimated corpus work across this many shards.',
    )
    parser.add_argument(
        '--shard-index',
        type=int,
        default=0,
        help='Zero-based shard of the sorted fuzz target list to execute.',
    )
    parser.add_argument(
        '--corpus-shards',
        type=int,
        default=1,
        help='For non-libFuzzer replay, split sufficiently large target corpora across this many processes.',
    )
    parser.add_argument(
        '--corpus-shard-min-files',
        type=int,
        default=750,
        help='Minimum input count before --corpus-shards splits a non-libFuzzer target corpus.',
    )
    parser.add_argument(
        'corpus_dir',
        help='The corpus to run on (must contain subfolders for each fuzz target).',
    )
    parser.add_argument(
        'target',
        nargs='*',
        help='The target(s) to run. Default is to run all targets.',
    )
    parser.add_argument(
        '--m_dir',
        action="append",
        help="Merge inputs from these directories into the corpus_dir.",
    )
    parser.add_argument(
        '-g',
        '--generate',
        action='store_true',
        help='Create new corpus (or extend the existing ones) by running'
             ' the given targets for a finite number of times. Outputs them to'
             ' the passed corpus_dir.'
    )

    args = parser.parse_args()
    args.corpus_dir = Path(args.corpus_dir)

    # Set up logging
    logging.basicConfig(
        format='%(message)s',
        level=int(args.loglevel) if args.loglevel.isdigit() else args.loglevel.upper(),
    )

    # Read config generated by configure.
    config = configparser.ConfigParser()
    configfile = os.path.abspath(os.path.dirname(__file__)) + "/../config.ini"
    config.read_file(open(configfile))

    if not config.getboolean("components", "ENABLE_FUZZ_BINARY"):
        logging.error("Must have fuzz executable built")
        sys.exit(1)

    fuzz_bin=os.getenv("CONNECTCOINFUZZ", default=os.path.join(config["environment"]["BUILDDIR"], 'bin', 'fuzz'))

    # Build list of tests
    test_list_all = parse_test_list(
        fuzz_bin=fuzz_bin,
        source_dir=config['environment']['SRCDIR'],
    )

    if not test_list_all:
        logging.error("No fuzz targets found")
        sys.exit(1)

    logging.debug("{} fuzz target(s) found: {}".format(len(test_list_all), " ".join(sorted(test_list_all))))

    args.target = args.target or test_list_all  # By default run all
    test_list_error = list(set(args.target).difference(set(test_list_all)))
    if test_list_error:
        logging.error("Unknown fuzz targets selected: {}".format(test_list_error))
    test_list_selection = list(set(test_list_all).intersection(set(args.target)))
    if not test_list_selection:
        logging.error("No fuzz targets selected")
    if args.exclude:
        for excluded_target in args.exclude.split(","):
            if excluded_target not in test_list_selection:
                logging.error("Target \"{}\" not found in current target list.".format(excluded_target))
                continue
            test_list_selection.remove(excluded_target)
    test_list_selection.sort()

    if args.shard_count < 1:
        parser.error('--shard-count must be at least 1')
    if not 0 <= args.shard_index < args.shard_count:
        parser.error('--shard-index must be between 0 and --shard-count - 1')
    if args.corpus_shards < 1:
        parser.error('--corpus-shards must be at least 1')
    if args.corpus_shard_min_files < 1:
        parser.error('--corpus-shard-min-files must be at least 1')
    test_list_selection, shard_loads = select_fuzz_shard(
        targets=test_list_selection,
        corpus_dir=args.corpus_dir,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
    )
    if not test_list_selection:
        parser.error('Selected fuzz shard contains no targets')

    load_description = ''
    if shard_loads is not None:
        load_description = ' (estimated corpus work units: {})'.format(shard_loads[args.shard_index])
    logging.info(
        "{} of {} detected fuzz target(s) selected for shard {}/{}{}: {}".format(
            len(test_list_selection),
            len(test_list_all),
            args.shard_index + 1,
            args.shard_count,
            load_description,
            " ".join(test_list_selection),
        )
    )

    if not args.generate:
        test_list_missing_corpus = []
        for t in test_list_selection:
            corpus_path = os.path.join(args.corpus_dir, t)
            if not os.path.exists(corpus_path) or len(os.listdir(corpus_path)) == 0:
                test_list_missing_corpus.append(t)
        test_list_missing_corpus.sort()
        if test_list_missing_corpus:
            logging.info(
                "Fuzzing harnesses lacking a corpus: {}".format(
                    " ".join(test_list_missing_corpus)
                )
            )
            logging.info(
                "Retain new ConnectCoin-specific corpora with the project issue or "
                "pull request until a project-owned corpus repository exists"
            )

    print("Check if using libFuzzer ... ", end='')
    help_output = subprocess.run(
        args=[
            fuzz_bin,
            '-help=1',
        ],
        env=get_fuzz_env(target=test_list_selection[0], source_dir=config['environment']['SRCDIR']),
        check=False,
        stderr=subprocess.PIPE,
        text=True,
    ).stderr
    using_libfuzzer = "libFuzzer" in help_output
    print(using_libfuzzer)
    if (args.generate or args.m_dir) and not using_libfuzzer:
        logging.error("Must be built with libFuzzer")
        sys.exit(1)

    with ThreadPoolExecutor(max_workers=args.par) as fuzz_pool:
        if args.generate:
            return generate_corpus(
                fuzz_pool=fuzz_pool,
                src_dir=config['environment']['SRCDIR'],
                fuzz_bin=fuzz_bin,
                corpus_dir=args.corpus_dir,
                targets=test_list_selection,
            )

        if args.m_dir:
            merge_inputs(
                fuzz_pool=fuzz_pool,
                corpus=args.corpus_dir,
                test_list=test_list_selection,
                src_dir=config['environment']['SRCDIR'],
                fuzz_bin=fuzz_bin,
                merge_dirs=[Path(m_dir) for m_dir in args.m_dir],
            )
            return

        run_once(
            fuzz_pool=fuzz_pool,
            corpus=args.corpus_dir,
            test_list=test_list_selection,
            src_dir=config['environment']['SRCDIR'],
            fuzz_bin=fuzz_bin,
            using_libfuzzer=using_libfuzzer,
            use_valgrind=args.valgrind,
            empty_min_time=args.empty_min_time,
            corpus_shards=args.corpus_shards,
            corpus_shard_min_files=args.corpus_shard_min_files,
        )


def transform_process_message_target(targets, src_dir):
    """Add a target per process message, and also keep ("process_message", {}) to allow for
    cross-pollination, or unlimited search"""

    p2p_msg_target = "process_message"
    if (p2p_msg_target, {}) in targets:
        lines = subprocess.run(
            ["git", "grep", "--function-context", "ALL_NET_MESSAGE_TYPES{", "src/protocol.h"],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
            cwd=src_dir,
        ).stdout.splitlines()
        lines = [l.split("::", 1)[1].split(",")[0].lower() for l in lines if l.startswith("src/protocol.h-    NetMsgType::")]
        assert len(lines)
        targets += [(p2p_msg_target, {"LIMIT_TO_MESSAGE_TYPE": m}) for m in lines]
    return targets


def transform_rpc_target(targets, src_dir):
    """Add a target per RPC command, and also keep ("rpc", {}) to allow for cross-pollination,
    or unlimited search"""

    rpc_target = "rpc"
    if (rpc_target, {}) in targets:
        lines = subprocess.run(
            ["git", "grep", "--function-context", "RPC_COMMANDS_SAFE_FOR_FUZZING{", "src/test/fuzz/rpc.cpp"],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
            cwd=src_dir,
        ).stdout.splitlines()
        lines = [l.split("\"", 1)[1].split("\"")[0] for l in lines if l.startswith("src/test/fuzz/rpc.cpp-    \"")]
        assert len(lines)
        targets += [(rpc_target, {"LIMIT_TO_RPC_COMMAND": r}) for r in lines]
    return targets


def generate_corpus(*, fuzz_pool, src_dir, fuzz_bin, corpus_dir, targets):
    """Generates new corpus.

    Run {targets} without input, and outputs the generated corpus to
    {corpus_dir}.
    """
    logging.info("Generating corpus to {}".format(corpus_dir))
    targets = [(t, {}) for t in targets]  # expand to add dictionary for target-specific env variables
    targets = transform_process_message_target(targets, Path(src_dir))
    targets = transform_rpc_target(targets, Path(src_dir))

    def job(command, t, t_env):
        logging.debug(f"Running '{command}'")
        logging.debug("Command '{}' output:\n'{}'\n".format(
            command,
            subprocess.run(
                command,
                env={
                    **t_env,
                    **get_fuzz_env(target=t, source_dir=src_dir),
                },
                check=True,
                stderr=subprocess.PIPE,
                text=True,
            ).stderr,
        ))

    futures = []
    for target, t_env in targets:
        target_corpus_dir = corpus_dir / target
        os.makedirs(target_corpus_dir, exist_ok=True)
        use_value_profile = int(random.random() < .3)
        command = [
            fuzz_bin,
            "-rss_limit_mb=8000",
            "-max_total_time=6000",
            "-reload=0",
            f"-use_value_profile={use_value_profile}",
            target_corpus_dir,
        ]
        futures.append(fuzz_pool.submit(job, command, target, t_env))

    for future in as_completed(futures):
        future.result()


def merge_inputs(*, fuzz_pool, corpus, test_list, src_dir, fuzz_bin, merge_dirs):
    logging.info(f"Merge the inputs from the passed dir into the corpus_dir. Passed dirs {merge_dirs}")
    jobs = []
    for t in test_list:
        args = [
            fuzz_bin,
            '-rss_limit_mb=8000',
            '-set_cover_merge=1',
            # set_cover_merge is used instead of -merge=1 to reduce the overall
            # size of the qa-assets git repository a bit, but more importantly,
            # to cut the runtime to iterate over all fuzz inputs [0].
            # [0] https://github.com/bitcoin-core/qa-assets/issues/130#issuecomment-1761760866
            '-shuffle=0',
            '-prefer_small=1',
            '-use_value_profile=0',
            # use_value_profile is enabled by oss-fuzz [0], but disabled for
            # now to avoid bloating the qa-assets git repository [1].
            # [0] https://github.com/google/oss-fuzz/issues/1406#issuecomment-387790487
            # [1] https://github.com/bitcoin-core/qa-assets/issues/130#issuecomment-1749075891
            os.path.join(corpus, t),
        ] + [str(m_dir / t) for m_dir in merge_dirs]
        os.makedirs(os.path.join(corpus, t), exist_ok=True)
        for m_dir in merge_dirs:
            (m_dir / t).mkdir(exist_ok=True)

        def job(t, args):
            output = 'Run {} with args {}\n'.format(t, " ".join(args))
            output += subprocess.run(
                args,
                env=get_fuzz_env(target=t, source_dir=src_dir),
                check=True,
                stderr=subprocess.PIPE,
                text=True,
            ).stderr
            logging.debug(output)

        jobs.append(fuzz_pool.submit(job, t, args))

    for future in as_completed(jobs):
        future.result()


def partition_corpus(*, corpus_path, shard_count, min_files):
    """Hardlink a large corpus into deterministic temporary shard directories."""
    corpus_files = sorted(path for path in corpus_path.iterdir() if path.is_file())
    if shard_count == 1 or len(corpus_files) < min_files:
        return [corpus_path], []

    shard_count = min(shard_count, len(corpus_files))
    temporary_directories = []
    try:
        for _ in range(shard_count):
            temporary_directories.append(
                tempfile.TemporaryDirectory(prefix=f'.{corpus_path.name}-shard-', dir=corpus_path.parent)
            )
        shard_paths = [Path(directory.name) for directory in temporary_directories]
        for index, corpus_file in enumerate(corpus_files):
            os.link(corpus_file, shard_paths[index % shard_count] / corpus_file.name)
    except OSError as error:
        for directory in temporary_directories:
            directory.cleanup()
        logging.warning("Unable to shard %s with hardlinks; replaying it serially: %s", corpus_path.name, error)
        return [corpus_path], []
    return shard_paths, temporary_directories


def run_once(
    *, fuzz_pool, corpus, test_list, src_dir, fuzz_bin, using_libfuzzer,
    use_valgrind, empty_min_time, corpus_shards, corpus_shard_min_files,
):
    jobs = []
    temporary_corpus_directories = []
    for t in test_list:
        corpus_path = corpus / t
        os.makedirs(corpus_path, exist_ok=True)
        empty_dir = not any(corpus_path.iterdir())
        if using_libfuzzer:
            input_groups = [[]]
            if empty_min_time and empty_dir:
                input_groups[0].append(f"-max_total_time={empty_min_time}")
            else:
                input_groups[0] += [
                    "-runs=1",
                    corpus_path,
                ]
        else:
            corpus_paths, temporary_directories = partition_corpus(
                corpus_path=corpus_path,
                shard_count=corpus_shards,
                min_files=corpus_shard_min_files,
            )
            temporary_corpus_directories.extend(temporary_directories)
            input_groups = [[path] for path in corpus_paths]
            if len(corpus_paths) > 1:
                logging.info(
                    "Split {} inputs for {} across {} replay processes".format(
                        sum(1 for path in corpus_path.iterdir() if path.is_file()),
                        t,
                        len(corpus_paths),
                    )
                )

        def job(t, args):
            output = 'Run {} with args {}'.format(t, args)
            result = subprocess.run(
                args,
                env=get_fuzz_env(target=t, source_dir=src_dir),
                stderr=subprocess.PIPE,
                text=True,
            )
            output += result.stderr
            return output, result, t

        for inputs in input_groups:
            args = [fuzz_bin, *inputs]
            if use_valgrind:
                args = ['valgrind', '--quiet', '--error-exitcode=1'] + args
            jobs.append(fuzz_pool.submit(job, t, args))

    stats = []
    failed = False
    first_exception = None
    try:
        for future in as_completed(jobs):
            try:
                output, result, target = future.result()
            except Exception as error:
                if first_exception is None:
                    first_exception = error
                continue
            logging.debug(output)
            try:
                result.check_returncode()
            except subprocess.CalledProcessError as e:
                if e.stdout:
                    logging.info(e.stdout)
                if e.stderr:
                    logging.info(e.stderr)
                logging.info(f"⚠️ Failure generated from target with exit code {e.returncode}: {result.args}")
                failed = True
                continue
            if using_libfuzzer:
                done_stat = [l for l in output.splitlines() if "DONE" in l]
                assert len(done_stat) == 1
                stats.append((target, done_stat[0]))
    finally:
        for directory in temporary_corpus_directories:
            directory.cleanup()

    if first_exception is not None:
        raise first_exception
    if failed:
        sys.exit(1)

    if using_libfuzzer:
        print("Summary:")
        max_len = max(len(t[0]) for t in stats)
        for t, s in sorted(stats):
            t = t.ljust(max_len + 1)
            print(f"{t}{s}")


def parse_test_list(*, fuzz_bin, source_dir):
    test_list_all = subprocess.run(
        fuzz_bin,
        env={
            'PRINT_ALL_FUZZ_TARGETS_AND_ABORT': '',
            **get_fuzz_env(target="", source_dir=source_dir)
        },
        stdout=subprocess.PIPE,
        text=True,
        check=True,
    ).stdout.splitlines()
    return test_list_all


if __name__ == '__main__':
    main()
