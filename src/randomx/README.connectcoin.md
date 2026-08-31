# Vendored RandomX

This directory vendors RandomX **v2.0.1**, upstream commit
`aaafe71322df6602c21a5c72937ac284724ae561`, from
<https://github.com/tevador/RandomX>.

RandomX is distributed under the BSD 3-Clause license in `LICENSE`. ConnectCoin
builds the library from source so that the consensus implementation is pinned
and cannot change because of a system-library upgrade.

The upstream CMake file has small integration changes locally:

- `RANDOMX_BUILD_TESTS` controls upstream helper executables.
- `RANDOMX_INSTALL` controls upstream install rules.
- The minimum CMake compatibility level is 3.10 to avoid relying on policies
  that current CMake versions have deprecated.
- The embedded target is always static, and MSVC ARM64 uses the portable
  interpreter instead of GNU AArch64 assembly.
- The MSVC assembly-generation rule tracks `configuration.h` explicitly.
- The hardware-AES probe uses a thread-local dummy value so concurrent VM
  creation does not race on upstream's process-global probe storage.

ConnectCoin disables both options and tests its wrapper and the official v2
reference vector in `src/test/randomx_tests.cpp`. CI also enables the
memory-intensive FAST/LIGHT equivalence vector on one 64-bit native job.

## ConnectCoin key schedule

Public networks use 2,048-block key epochs and a 64-block lag. For candidate
height `h`, the key-block height is
`floor((h - 64) / 2048) * 2048` once `h > 64`; height zero selects the
network-specific bootstrap key. Consequently, the block at height 2,048 first
becomes the key for the candidate at height 2,112. Regtest uses its fixed
bootstrap key for every height. This schedule is ConnectCoin consensus and is
deliberately documented rather than inferred from another RandomX user.
