### Verify Binaries

> [!WARNING]
> This inherited utility verifies upstream Bitcoin Core release artifacts from
> Bitcoin Core servers and signing keys. It does **not** verify ConnectCoin
> builds. ConnectCoin has no public release-signing infrastructure yet.

> [!WARNING]
> The current implementation counts all cryptographically good signatures
> toward `--min-good-sigs`, including signatures classified as untrusted.
> `--trusted-keys` affects the report's classification, not that count. A
> successful exit does not prove that a threshold of trusted signers was met;
> independently check the reported signer fingerprints against your trust policy.

#### Preparation

As of Bitcoin Core v22.0, releases are signed by a number of public keys on the basis
of the [guix.sigs repository](https://github.com/bitcoin-core/guix.sigs/). When
verifying binary downloads, you (the end user) decide which of these public keys you
trust and then use that trust model to evaluate the signature on a file that contains
hashes of the release binaries. The downloaded binaries are then hashed and compared to
the signed checksum file.

First, you have to figure out which public keys to recognize. Browse the [list of frequent
builder-keys](https://github.com/bitcoin-core/guix.sigs/tree/main/builder-keys) and
decide which of these keys you would like to trust. For each key you want to trust, you
must obtain that key for your local GPG installation.

You can obtain these keys by
  - through a browser using a key server (e.g. keyserver.ubuntu.com),
  - manually using the `gpg --keyserver <url> --recv-keys <key>` command, or
  - you can run the packaged `verify.py --import-keys ...` script to
    have it automatically retrieve unrecognized keys.

#### Usage

This script attempts to download the checksum file (`SHA256SUMS`) and corresponding
signature file `SHA256SUMS.asc` from https://bitcoincore.org and https://bitcoin.org.

It first checks the checksum file's signatures against `--min-good-sigs` (3 by
default; subject to the trust limitation above), and
then downloads the release files specified in the checksum file, and checks if the
hashes of the release files are as expected.

If we encounter pubkeys in the signature file that we do not recognize, the script
can prompt the user as to whether they'd like to download the pubkeys. To enable
this behavior, use the `--import-keys` flag.

The script returns 0 when its checks pass, 1 for an integrity failure, and 9
when there are too few good signatures. Other failures have other nonzero
codes; see `ReturnCode` in `verify.py`. In local `bin` mode, missing files can
still produce exit code 0, as explained below.

Run `verify.py --help` and each subcommand's `--help` for options. Defaults can
also be set through the `BINVERIFY_*` environment variables in `verify.py`.

#### Examples

Validate releases with default settings:
```sh
./contrib/verify-binaries/verify.py pub 22.0
./contrib/verify-binaries/verify.py pub 22.0-rc3
```

Get JSON output and don't prompt for user input (no auto key import):

```sh
BINVERIFY_IMPORTKEYS=0 ./contrib/verify-binaries/verify.py --json pub 22.0-x86
BINVERIFY_IMPORTKEYS=0 ./contrib/verify-binaries/verify.py --json pub 23.0-rc5-linux-gnu
```

Classify the specified keys as trusted in the report, while requiring at least
10 good signatures in total (not necessarily 10 trusted signatures):
```sh
./contrib/verify-binaries/verify.py \
    --trusted-keys 74E2DEF5D77260B98BC19438099BAD163C70FBFA,9D3CC86A72F8494342EA5FD10A41BDC3F4FAFF1C \
    --min-good-sigs 10 pub 22.0-linux
```

If you only want to download the binaries for a certain architecture and/or platform, add the corresponding suffix, e.g.:

```sh
./contrib/verify-binaries/verify.py pub 25.2-x86_64-linux
./contrib/verify-binaries/verify.py pub 24.1-rc1-darwin
./contrib/verify-binaries/verify.py pub 27.0-win64-setup.exe
```

If you do not want to keep the downloaded binaries, specify the cleanup option.

```sh
./contrib/verify-binaries/verify.py pub --cleanup 22.0
```

Use the `bin` subcommand to verify locally present files listed in a checksum
file. Without explicit binary paths, it looks beside the checksum file, reports
absent files as `MISSING` (or `missing_binaries` in JSON), and can still return 0.
Check that report; this mode does not guarantee that every listed file was
present and verified. The checksum signature file is also required, normally
`SHA256SUMS.asc` beside `SHA256SUMS`, or supplied with `--sums-sig-file`.

```sh
./contrib/verify-binaries/verify.py bin SHA256SUMS
```

To require verification of particular downloads, pass their paths explicitly:

```sh
./contrib/verify-binaries/verify.py bin ~/Downloads/SHA256SUMS \
    ~/Downloads/bitcoin-24.0.1-x86_64-linux-gnu.tar.gz \
    ~/Downloads/bitcoin-24.0.1-arm-linux-gnueabihf.tar.gz
```
