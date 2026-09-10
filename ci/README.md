# CI Scripts

This directory contains scripts for each build step in each build stage.

## Running a Stage Locally

Be aware that the tests will be built and run in-place, so please run at your own risk.
If the repository is not a fresh git clone, you might have to clean files from previous builds or test runs first.

The ci needs to perform various sysadmin tasks such as installing packages or writing to the user's home directory.
While it should be fine to run
the ci system locally on your development box, the ci scripts can generally be assumed to have received less review and
testing compared to other parts of the codebase. If you want to keep the work tree clean, you might want to run the ci
system in a virtual machine with a Linux operating system of your choice.

To allow for a wide range of tested environments, but also ensure reproducibility to some extent, the test stage
requires `bash`, `docker`, and `python3` to be installed. To run on different architectures than the host `qemu` is also required. To install all requirements on Ubuntu, run

```
sudo apt install bash docker.io python3 qemu-user-static
```

For some sanitizer builds, the kernel's address-space layout randomization
(ASLR) entropy can cause sanitizer shadow memory mappings to fail. When running
the CI locally you may need to reduce that entropy by running:

```
sudo sysctl -w vm.mmap_rnd_bits=28
```

To run a test that requires emulating a CPU architecture different from the
host, we may rely on the container environment recognizing foreign executables
and automatically running them using `qemu`. The following sets us up to do so
(also works for `podman`):

```
docker run --rm --privileged docker.io/multiarch/qemu-user-static --reset -p yes
```

It is recommended to run the CI system in a clean environment. The `env -i`
command below ensures that *only* specified environment variables are propagated
into the local CI.
To run the test stage with a specific configuration:

```
env -i HOME="$HOME" PATH="$PATH" USER="$USER" FILE_ENV="./ci/test/00_setup_env_arm.sh" ./ci/test_run_all.sh
```

## Configurations

The test files (`FILE_ENV`) are constructed to test a wide range of
configurations, rather than a single pass/fail. This helps to catch build
failures and logic errors that present on platforms other than the ones the
author has tested.

Some builders use the dependency-generator in `./depends`, rather than using
the system package manager to install build dependencies. This guarantees that
the tester is using the same versions as the release builds, which also use
`./depends`.

It is also possible to force a specific configuration without modifying the
file. For example,

```
env -i HOME="$HOME" PATH="$PATH" USER="$USER" MAKEJOBS="-j1" FILE_ENV="./ci/test/00_setup_env_arm.sh" ./ci/test_run_all.sh
```

The files starting with `0n` (`n` greater than 0) are the scripts that are run
in order.

## Cache

In order to avoid rebuilding all dependencies for each build, the binaries are
cached and reused when possible. Changes in the dependency-generator will
trigger cache-invalidation and rebuilds as necessary.

## Fuzz replay scheduling

The Linux ASan and MSan fuzz jobs use `ci/fuzz-timings.json` to balance targets
between shards and start expensive targets first. These are summed process
seconds from a completed CI run, not whole-job wall times. They only change
scheduling: every selected target and corpus input still runs, with the same
corpus partitions, worker limit, replay arguments, and empty-corpus mutation
budget. Targets added after a measurement are retained and estimated from their
corpus size and file count.

Each profile records its source run, exact QA-assets commit, fuzz engine, and
SHA256 of the corresponding setup script (UTF-8 with normalized LF line endings).
The runner checks the actual corpus checkout and detected engine before using it.
A run without a profile uses the corpus-work estimator. An invalid profile may
fall back only for a single-shard run; a requested profile that cannot be
validated in a multi-shard run is a fatal error. Otherwise, a transient failure
on just one runner could select a different partition and silently miss targets.

To refresh a profile, sum all successful `Finished TARGET in ...s` measurements
for each target, including every corpus partition, from the same completed run
and configuration. A value below the log clock's resolution is recorded as
0.1 seconds, not used to reduce execution. Update its provenance and configuration
hash, and run `python3 test/lint/lint-fuzz-corpus.py`. When changing the pinned
corpus or setup flags, either collect fresh timings or remove both timing
arguments from that configuration so that **all** its shards use the original
estimator together. Do not bypass a mismatched profile on just one shard.

## Configuring a repository for CI

The checked-in workflow uses GitHub-hosted runners and GitHub Actions caches for
every repository. No runner, cache, or organization owned by an upstream project
is selected based on the repository name.

Repositories that configure project-owned runners may change the `runs-on`
values and cache provider deliberately. Keep a GitHub-hosted fallback until the
new runners have been validated, and never reference infrastructure that the
ConnectCoin project does not control.
