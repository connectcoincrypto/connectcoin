Contents
========
This directory contains tools for developers working on this repository.

deterministic-fuzz-coverage
===========================

A tool to check for non-determinism in fuzz coverage. To get the help, run:

```
cargo run --manifest-path ./contrib/devtools/deterministic-fuzz-coverage/Cargo.toml -- --help
```

To execute the tool, compilation has to be done with the build options:

```
-DCMAKE_C_COMPILER='clang' -DCMAKE_CXX_COMPILER='clang++' -DBUILD_FOR_FUZZING=ON -DCMAKE_CXX_FLAGS='-fprofile-instr-generate -fcoverage-mapping'
```

Both llvm-profdata and llvm-cov must be installed. Also, the qa-assets
repository must have been cloned. Finally, a fuzz target has to be picked
before running the tool:

```
cargo run --manifest-path ./contrib/devtools/deterministic-fuzz-coverage/Cargo.toml -- $PWD/build_dir $PWD/qa-assets/fuzz_corpora fuzz_target_name
```

deterministic-unittest-coverage
===========================

A tool to check for non-determinism in unit-test coverage. To get the help, run:

```
cargo run --manifest-path ./contrib/devtools/deterministic-unittest-coverage/Cargo.toml -- --help
```

To execute the tool, compilation has to be done with the build options:

```
-DCMAKE_C_COMPILER='clang' -DCMAKE_CXX_COMPILER='clang++' -DCMAKE_CXX_FLAGS='-fprofile-instr-generate -fcoverage-mapping'
```

Both llvm-profdata and llvm-cov must be installed.

```
cargo run --manifest-path ./contrib/devtools/deterministic-unittest-coverage/Cargo.toml -- $PWD/build_dir <boost unittest filter>
```

clang-format-diff.py
===================

A script to format unified git diffs according to [.clang-format](../../src/.clang-format).

Requires `clang-format`, installed e.g. via `brew install clang-format` on macOS,
or `sudo apt install clang-format` on Debian/Ubuntu.

For instance, to format the last commit with 0 lines of context,
the script should be called from the git root folder as follows.

```
git diff -U0 HEAD~1.. | ./contrib/devtools/clang-format-diff.py -p1 -i -v
```

gen-manpages.py
===============

A small script to automatically create manpages in ../../doc/man by running the release binaries with the -help option.
This requires help2man which can be found at: https://www.gnu.org/software/help2man/

This script assumes a build directory named `build` as suggested by example build documentation.
To use it with a different build directory, set `BUILDDIR`.
For example:

```bash
BUILDDIR=$PWD/my-build-dir contrib/devtools/gen-manpages.py
```

headerssync-params.py
=====================

A script to generate optimal parameters for the headerssync module (stored in
`src/kernel/chainparams.cpp`). Network-dependent values are mandatory so the
tool cannot silently reuse another chain's genesis or minimum-chain-work data.
It runs many times faster inside PyPy. Example invocation for the current
development mainnet parameters (review the target date and chain-work policy
before using generated values in a release):

```bash
pypy3 contrib/devtools/headerssync-params.py \
  --target-date 2031-08-25 \
  --genesis-time 1787596781 \
  --minchainwork-headers 1 \
  --block-interval-seconds 600
```

gen-connectcoin-conf.sh
===================

Generates a connectcoin.conf file in `share/examples/` by parsing the output from `connectcoind --help`. This script is run during the
release process to include a connectcoin.conf with the release binaries and can also be run by users to generate a file locally.
When generating a file as part of the release process, make sure to commit the changes after running the script.

This script assumes a build directory named `build` as suggested by example build documentation.
To use it with a different build directory, set `BUILDDIR`.
For example:

```bash
BUILDDIR=$PWD/my-build-dir contrib/devtools/gen-connectcoin-conf.sh
```

circular-dependencies.py
========================

Run this script from the root of the source tree (`src/`) to find circular dependencies in the source code.
This looks only at which files include other files, treating the `.cpp` and `.h` file as one unit.

Example usage:

    cd .../src
    ../contrib/devtools/circular-dependencies.py {*,*/*,*/*/*}.{h,cpp}
