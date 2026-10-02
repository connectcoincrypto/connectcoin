# Linux release packaging

The `Linux installers` Actions workflow builds x86_64 installers from the exact
checked-out commit. It saves SHA-256 checksums and a build manifest, then tests
installation, execution and removal in clean containers. It does **not** publish
or replace GitHub Release assets.

- **DEB:** built on Ubuntu 22.04 (GCC 12 / Qt 6.2), tested on Ubuntu 22.04,
  Ubuntu 24.04 and Debian 12. Corresponding Mint versions can use the DEB;
  Mint desktop integration is not separately tested in this workflow.
- **RPM:** built and tested on Fedora 43. This is not a universal RPM for older
  RHEL, openSUSE, or every other RPM distribution.
- **AppImage / portable tarball:** bundled Qt, Ubuntu 22.04 baseline. See below.

Native packages include the seven release applications, desktop launcher, icon,
man pages and dependency licenses. They do not install a service, change a
firewall, start mining, or touch user wallets, data or configuration. Uninstalling
removes program files only. Experimental multiprocess IPC is disabled; the
monolithic GUI and daemon remain fully functional.

Every format also includes an optional `connectcoin-hugepages` setup command.
DEB/RPM installation has no configuration wizard or privileged post-install
hook: opt in explicitly afterward. The command needs the distribution's Python
3 (recommended/suggested, not required by Core), and supports read-only `status`,
confirmed administrator `apply`, and conservative `restore`. It only changes
runtime HugeTLB settings for the current boot; it never starts Core or mining.
See [the Huge Pages setup guide](HUGE-PAGES.md) for sizing, group permissions,
backup/recovery, portable usage and the offline regression tests.

## Native package build

Install the build dependencies from `doc/build-unix.md`, `help2man`, and `dpkg-dev`
for DEB or `rpm-build` for RPM. Use a compiler meeting `doc/dependencies.md`.

```sh
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_GUI=ON -DBUILD_TX=ON -DBUILD_UTIL=ON -DBUILD_WALLET_TOOL=ON \
  -DENABLE_IPC=OFF -DWITH_ZMQ=ON -DENABLE_LINUX_PACKAGING=ON \
  -DCPACK_GENERATOR=DEB -DCONNECTCOIN_PACKAGE_RELEASE=1ubuntu22.04
cmake --build build-linux -j2
(cd build-linux && cpack --config CPackConfig.cmake)
```

For Fedora, select `-DCPACK_GENERATOR=RPM` and a revision such as
`-DCONNECTCOIN_PACKAGE_RELEASE=1.fc43`. Native packages retain system library
dependencies; they do not replace or bundle the system Qt installation.
The `linux-manpages` target generates real manuals from the compiled tools;
source-tree placeholder manuals are never shipped in these native packages.
Install with `sudo apt install ./<filename>.deb` or
`sudo dnf install ./<filename>.rpm` after verifying the release checksums.

The workflow is manually dispatchable and also runs on the explicit
`codex/linux-installers` packaging branch. Ordinary `main` commits do not
automatically publish new installers.

## Portable bundles

Run `build-portable.sh` on Linux x86_64 after installing the release runtime
components into a private staging prefix. Ubuntu 22.04 is the build baseline;
the helper rejects bundled ELF files requiring glibc newer than 2.35. This
does not make the bundle compatible with every distribution: graphics drivers,
desktop integration and libraries on linuxdeploy's exclusion list still come
from the host. musl-only distributions are unsupported.

The portable GUI requires the host's OpenGL/EGL driver stack, font libraries,
and X11/Wayland client libraries, including for `QT_QPA_PLATFORM=minimal` help
and version checks. These are intentional linuxdeploy exclusions; copying
graphics drivers from the build machine into the bundle can break other hosts.
An ordinary desktop usually already supplies them. For a minimal Ubuntu/Debian
container or installation, install the following runtime packages:

```sh
sudo apt-get install --no-install-recommends \
  libfontconfig1 libfreetype6 libharfbuzz0b libgl1 libegl1 libopengl0 \
  libwayland-client0 libx11-6 libx11-xcb1 libxcb1 libice6 libsm6
```

For Fedora 43:

```sh
sudo dnf install fontconfig freetype harfbuzz mesa-libGL mesa-libEGL \
  libglvnd-egl libglvnd-opengl libglvnd-glx libwayland-client \
  libX11 libX11-xcb libxcb libICE libSM
```

Fedora's `mesa-libEGL` supplies the Mesa vendor implementation; `libglvnd-egl`
supplies `libEGL.so.1`. Both are listed so minimal systems receive the public
EGL loader and a driver. The package managers install the corresponding
dispatch libraries and graphics dependencies. Keep the normal distribution
graphics drivers on machines using another GPU vendor.

The bundle also uses base-system glibc, C++/GCC runtime, zlib, Expat,
libcom_err, libgpg-error, and libuuid. In the test distributions these are
already installed or pulled in by the packages above and the test harness.
On more minimal root filesystems their package names are:

| Ubuntu / Debian | Fedora 43 |
| --- | --- |
| `libc6`, `libstdc++6`, `libgcc-s1` | `glibc`, `libstdc++`, `libgcc` |
| `zlib1g`, `libexpat1` | `zlib-ng-compat`, `expat` |
| `libcom-err2`, `libgpg-error0`, `libuuid1` | `libcom_err`, `libgpg-error`, `libuuid` |

These requirements come from the libraries explicitly excluded in the release
build log. See the [upstream exclusion list](https://github.com/AppImageCommunity/pkg2appimage/blob/master/excludelist)
and the [Fedora EGL loader package](https://packages.fedoraproject.org/pkgs/libglvnd/libglvnd-egl/fedora-43.html).
The portable tests must install this host runtime before checking the bundle,
without installing the system Qt packages that the bundle is meant to supply.

```sh
bash contrib/linuxdeploy/build-portable.sh \
  --staging-prefix "$PWD/stage/usr" \
  --output-dir "$PWD/dist" \
  --version 1.0.1 \
  --qmake /usr/bin/qmake6
```

The prefix must contain all seven `connectcoin`, `connectcoin-qt`, `connectcoind`,
`connectcoin-cli`, `connectcoin-wallet`, `connectcoin-tx`, and `connectcoin-util`
ELF executables under `bin/`. Stage the Core documentation and bundled dependency
licenses too. The prefix is copied before any library patching. Do not pass
`/usr`, a live installation, or your data directory. Outputs must be in a
separate directory and existing artifacts with the same version are refused.

The default creates both `ConnectCoin-Core-<version>-x86_64.AppImage` and
`connectcoin-<version>-linux-x86_64.tar.gz`, plus the unpacked directory.
`--format tar` and `--format appimage` select a single archive format. The
unpacked directory is always kept. Both formats include relative launchers for
the GUI and every command-line tool; the AppImage also accepts
`--tool connectcoin-cli -help`, for example. See `PORTABLE-README.txt` for users.

## Packaging prerequisites

Besides the build's dependencies, install these Ubuntu packages:

```sh
sudo apt-get install ca-certificates curl file binutils desktop-file-utils \
  qmake6 qt6-base-dev-tools qt6-qpa-plugins qt6-translations-l10n qt6-wayland
```

`qt6-wayland` is optional; its platform plugins are included when present.
The X11 and minimal platform plugins are required. Minimal supports the headless
GUI smoke checks. GNU coreutils, findutils, grep, sed, tar, and gzip are also
required. The helper itself runs without root. Neither packaging nor smoke
testing requires FUSE: `APPIMAGE_EXTRACT_AND_RUN=1` is set for packaging tools.
No Linux binaries are downloaded or run on Windows.

Use `smoke_test.py --portable --prefix <unpacked-directory>`. Also test
the outer launchers after moving the extracted bundle to a different directory,
including a path with spaces. Set `QT_QPA_PLATFORM=minimal` for headless GUI help
or version checks. The GitHub workflow runs the real Linux build and smoke tests;
`bash -n` on Windows checks syntax only. Native packages use
`smoke_test.py --prefix /usr`. Both modes isolate all wallet and profile data
and run a temporary regtest daemon. Headless checks do not replace testing on
a real X11/Wayland desktop.

The packaging workflow sets `QT_RCC_SOURCE_DATE_OVERRIDE=1` to give embedded
Qt resource metadata a stable timestamp, following the policy in
`depends/patches/qt/rcc_hardcode_timestamp.patch`. This allows compiler-cache
reuse when translation contents are unchanged but generated `.qm` file mtimes
differ between builds. `test-rcc-timestamps.sh` exercises the installed Qt 6
resource compiler with identical payloads and different mtimes, and checks that
real content changes still alter its output. Compiler-cache statistics are
reset immediately before compilation and reported afterward; all unit tests
and runtime checks remain enabled.

## Download pins

All executable downloads are SHA256 checked before execution. The official
GitHub API release asset `digest` fields were verified on 2026-10-01:

| Asset | Release / source revision | Official asset ID | SHA256 |
| --- | --- | --- | --- |
| linuxdeploy x86_64 | `1-alpha-20251107-1`, `cc7b86472c3caa3fd729b9dc502fd2aa78394257` | `313839329` | `c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d` |
| linuxdeploy Qt plugin x86_64 | continuous asset, release target `9b9fca179e7312ae4a433ad6c3183d73c116180b` | `525032210` | `cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617` |
| AppImage type2 runtime x86_64 | `20251108`, `dd6cebedcbddde9c82f89b011e8e1d40b6e43868` | `326011592` | `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d` |

The Qt release's moving `continuous` URL is deliberately not used: the script
addresses the exact asset ID and verifies its hash. If upstream deletes that
asset, the build fails until the reviewed pin is updated. Do not bypass hash
verification to recover such a build. For pin upgrades, inspect the official
release, asset metadata, source changes, and run the full packaging/smoke workflow.
The AppImage runtime is passed through `LDAI_RUNTIME_FILE` so appimagetool does
not download an unpinned runtime implicitly. The output plugin and appimagetool
are those bundled inside the pinned linuxdeploy AppImage.

Primary references:

- [linuxdeploy release](https://github.com/linuxdeploy/linuxdeploy/releases/tag/1-alpha-20251107-1)
- [Qt plugin documentation](https://github.com/linuxdeploy/linuxdeploy-plugin-qt)
- [AppImage output plugin configuration](https://github.com/linuxdeploy/linuxdeploy-plugin-appimage)
- [AppImage runtime release](https://github.com/AppImage/type2-runtime/releases/tag/20251108)
- [FUSE-free AppImage operation](https://docs.appimage.org/user-guide/troubleshooting/fuse.html)
