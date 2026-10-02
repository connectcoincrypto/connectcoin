# macOS Deployment

## Native macOS 15+ DMG artifacts

The native installer workflow builds separate Intel (`x86_64`) and Apple
Silicon (`arm64`) disk images. Open the matching DMG and drag **ConnectCoin
Core.app** to **Applications**. The disk image does not install a service,
modify wallet/node data, or install command-line tools into a system directory.
This DMG contains the complete graphical full node and wallet, not a separate
set of command-line executables.

### Huge Pages: automatic request, no installer configuration

There is **no Huge Pages enable/disable option in the macOS installer**. The
bundled RandomX backend already requests `VM_FLAGS_SUPERPAGE_SIZE_2MB` through
anonymous `mmap` and falls back to regular pages when allocation fails. No
Windows-style account privilege or Linux-style page reservation is appropriate
for this backend on macOS.

The [Apple XNU macOS 15 implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.1.10/osfmk/vm/vm_map.c)
accepts this superpage size only for `__x86_64__`; it rejects the request on
Apple Silicon. Thus `regular_pages` is expected on that ARM64 implementation,
not evidence of an installation error. Intel allocation can still fail due to
memory availability or other allocation constraints. The
[mmap implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.1.10/bsd/kern/kern_mman.c)
passes the special anonymous-map flags to Mach. These are allocation semantics,
not settings for the installer to change.

The DMG's `INSTALL.txt` includes [HUGE-PAGES.txt](HUGE-PAGES.txt), which is also
kept at `ConnectCoin Core.app/Contents/Resources/HUGE-PAGES.txt` after the app is
copied to Applications. It explains the Mining page snapshot and the read-only
`getcpumininginfo` RPC's `randomx_dataset` field. After choosing to start mining,
wait for preparation before interpreting that result. It describes the shared
dataset's allocation path, not cache/scratchpads/JIT or a measured physical page
size. Regular-page and LIGHT fallbacks remain valid, but may be slower.

Do not run the wallet as root, disable SIP or Gatekeeper, modify JIT protections,
or add signing entitlements to pursue Huge Pages. The DMG never performs those
changes. Build and verification reports record this no-configuration policy
and the instructions' SHA-256; their help/version smoke does **not** allocate
a dataset or certify that Huge Pages will work on the destination computer.

### Signing and native dependencies

These artifacts are **ad-hoc signed**, not signed with a project-owned Apple
Developer ID and not notarized by Apple. Signature verification checks bundle
integrity; it does not establish an identified publisher or Gatekeeper approval.
macOS may block an Internet-downloaded application. Do not disable Gatekeeper
globally. The `connectcoin-signing-disabled` marker remains in place until the
project establishes its signing identity and release policy.

The native build uses Xcode 16.2, a **15.0 deployment target**, and the repository's
pinned static `depends` libraries, including Qt 6.8.4. The deployment target is
not an SDK pin: the SDK comes from the selected Xcode and its actual version is
recorded in the companion source archive. Apple SDKs are not redistributed.
The native `depends/builders/darwin.mk` uses `xcrun`; the extracted SDK procedure
below is for cross-compilation, not these native jobs.

On the matching native architecture, the dependency build is:

```bash
brew install cmake make ninja pkgconf python coreutils
native_host=$(./depends/config.sub "$(./depends/config.guess)")
gmake -C depends -j2 NO_IPC=1 NO_USDT=1 XCODE_VERSION=16.2 \
  OSX_MIN_VERSION=15.0 OSX_SDK="$(xcrun --show-sdk-path)" \
  OSX_SDK_VERSION="$(xcrun --show-sdk-version)"
cmake -S . -B build-macos -G Ninja \
  --toolchain "depends/$native_host/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DBUILD_GUI=ON -DENABLE_IPC=OFF -DWITH_USDT=OFF
cmake --build build-macos -j2 --target connectcoin-qt
```

Do not replace the native triple with a shortened `arm64-apple-darwin` or
`x86_64-apple-darwin` value: `depends` compares the build and host triples
literally when selecting its native Qt tools. A cold Qt build can take time;
the pinned dependency cache is not a substitute for corresponding source.

### Corresponding source and license notices

Keep the companion `connectcoin-1.0.1-macos-<arch>-sources.tar.gz` available beside
each DMG. The application is not an MIT-only redistribution: its Qt, QRencode,
ZeroMQ, and other dependencies retain their respective upstream licenses.
Statically linked libraries must remain modifiable and relinkable; the source
artifact includes the inputs and instructions needed for rebuilding rather than
only license identifiers. See [Qt's open-source obligations](https://www.qt.io/development/open-source-lgpl-obligations)
and the actual upstream license texts included with the package.

After a successful build from a clean committed checkout, collect sources
**before** constructing the disk image:

```bash
gmake -C depends download-one NO_IPC=1 NO_USDT=1 XCODE_VERSION=16.2 \
  OSX_MIN_VERSION=15.0 OSX_SDK="$(xcrun --show-sdk-path)" \
  OSX_SDK_VERSION="$(xcrun --show-sdk-version)"
python3 contrib/macdeploy/collect_sources.py \
  --repo . --build-dir build-macos --output-dir build-macos/dist \
  --version 1.0.1 --arch "$(uname -m)"
python3 contrib/macdeploy/build_dmg.py \
  --build-dir build-macos --output-dir build-macos/dist \
  --version 1.0.1 --arch "$(uname -m)" --minimum-macos 15.0 \
  --qt-translations "depends/$native_host/translations" \
  --licenses-dir "build-macos/dist/licenses-$(uname -m)"
python3 contrib/macdeploy/validate_dmg.py \
  --dmg "build-macos/dist/connectcoin-core-1.0.1-macos-$(uname -m).dmg" \
  --version 1.0.1 --arch "$(uname -m)" \
  --output "build-macos/dist/verification-$(uname -m).json"
```

The helper prints JSON with `source_archive`, `licenses_dir`, and
`source_commit`. Supply the reported directory to `build_dmg.py` through its
`--licenses-dir` argument. It contains `COPYRIGHT.txt`, `THIRD-PARTY.txt`,
`REBUILD.txt`, and individually preserved notices indexed by
`LICENSE-MANIFEST.json`. The source archive contains:

- `source/connectcoin/`: `git archive HEAD`, including dependency recipes,
  local patches, and packaging tools;
- `source/depends-sources/`: the exact source archives and Qt build-support
  files, checked against their recipe SHA-256 hashes;
- `source/mbedtls/`: the complete populated FetchContent source, including
  ConnectCoin modifications but excluding `.git` metadata;
- `licenses/`, `REBUILD.txt`, and `build-environment.json`: notices, original
  commit, source hashes, selected build settings, and SDK/compiler information.

The helper reads archive members without extracting them, rejects unsafe paths,
links escaping their source root, special files, duplicate entries, and missing
or mismatched inputs. License filenames are flattened with an origin hash to
avoid archive-controlled filesystem paths. It does not download dependencies,
run a build, include Apple SDKs, or execute dependency code. Existing output
names are never overwritten; use a fresh output directory for another build.

To rebuild from the source artifact, follow its `REBUILD.txt`; the supplied
Mbed TLS source can be selected with CMake's
`FETCHCONTENT_SOURCE_DIR_CONNECTCOIN_MBEDTLS` option. The archived repository
does not include `.git`. If packaging a rebuilt or modified version with the
Git-checking tools, initialize a new local repository, commit that source, and
collect a **new** source artifact and licenses directory before packaging. Its
new commit identifies that local rebuild, not the original release. The scripts
do not claim bit-for-bit reproducibility across different SDKs or build tools.

Collector regression tests run without macOS or a dependency build:

```bash
python3 contrib/macdeploy/collect_sources.py --self-test
```

## Legacy app archive target

The `macdeployqtplus` script should not be run manually. Instead, after building as usual:

```bash
cmake --build build --target deploy
```

When complete, it will have produced a `ConnectCoin-Qt` application archive.

## SDK Extraction

### Step 1: Obtaining `Xcode.app`

A free Apple Developer Account is required to proceed.

Our macOS SDK can be extracted from
[Xcode_26.1.1_Apple_silicon.xip](https://download.developer.apple.com/Developer_Tools/Xcode_26.1.1/Xcode_26.1.1_Apple_silicon.xip).

Alternatively, after logging in to your account go to 'Downloads', then 'More'
and search for [`Xcode 26.1.1`](https://developer.apple.com/download/all/?q=Xcode%2026.1.1).

An Apple ID and cookies enabled for the hostname are needed to download this.

The `sha256sum` of the downloaded XIP archive should be `f4c65b01e2807372b61553c71036dbfef492d7c79d4c380a5afb61aa1018e555`.

To extract the `.xip` on Linux:

```bash
# Install/clone tools needed for extracting Xcode.app
apt install cpio
git clone https://github.com/bitcoin-core/apple-sdk-tools.git

# Unpack the .xip and place the resulting Xcode.app in your current
# working directory
python3 apple-sdk-tools/extract_xcode.py -f Xcode_26.1.1_Apple_silicon.xip | cpio -d -i
```

On macOS:

```bash
xip -x Xcode_26.1.1_Apple_silicon.xip
```

### Step 2: Generating the SDK tarball from `Xcode.app`

To generate the SDK, run the script [`gen-sdk.py`](./gen-sdk.py) with the
path to `Xcode.app` (extracted in the previous stage) as the first argument.

```bash
./contrib/macdeploy/gen-sdk.py '/path/to/Xcode.app'
```

The generated archive should be: `Xcode-26.1.1-17B100-extracted-SDK-with-libcxx-headers.tar`.
The `sha256sum` should be `9600fa93644df674ee916b5e2c8a6ba8dacf631996a65dc922d003b98b5ea3b1`.

## Deterministic macOS App Notes

macOS Applications are created on Linux using a recent LLVM.

All builds must target an Apple SDK. These SDKs are free to download, but not redistributable.
See the SDK Extraction notes above for how to obtain it.

The Guix build process has been designed to avoid including the SDK's files in Guix's outputs.
All interim tarballs are fully deterministic and may be freely redistributed.

Using an Apple-blessed key to sign binaries is a requirement to produce (distributable) macOS
binaries. Because this private key cannot be shared, we'll have to be a bit creative in order
for the build process to remain somewhat deterministic. Here's how it works:

- Builders use Guix to create an unsigned release. This outputs an unsigned ZIP which
  users may choose to bless, self-codesign, and run. It also outputs an unsigned app structure
  in the form of a tarball.
- After ConnectCoin establishes a project-owned Apple identity and release
  policy, its authorized keyholder can use the included script to create a
  detached signature. The script currently refuses to run while
  `connectcoin-signing-disabled` exists.
- Future release builders can feed the unsigned app and an authenticated
  detached signature back into Guix to produce a deterministic ZIP.
