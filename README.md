ConnectCoin Core development tree
=================================

ConnectCoin Core is a Bitcoin Core fork under active development. Its goal is a
UTXO cryptocurrency with native pay-to-connect (P2C) outputs backed by
independently verifiable TLS 1.3 connection proofs.

- [ConnectCoin Community](https://discord.gg/JYWbz5PsPp)
- [Blockchain Explorer](https://explorer.connectcoincrypto.com/)
- [Whitepaper](https://connectcoincrypto.com/whitepaper.pdf)

This repository is not production-ready. The current consensus milestone uses
typed transaction outputs instead of serialized output scripts. Type `1` is a
single 32-byte x-only public key authorized by one 64-byte BIP340 Schnorr
signature. Type `2` is PAY_TO_CONNECT for a canonical DNS domain and is spent
with a bounded, independently verified TLS 1.3 connection proof. There is no
certificate-specific P2C output form.

Mainnet has **not** been launched and has no genesis block in the node. The beta
defaults to `testnet4` without requiring a configuration file or network flag.
Use `-regtest` for local testing; explicit mainnet startup is deliberately rejected. Test coins do
not become mainnet coins. See [doc/testnet-beta.md](doc/testnet-beta.md) for the
network boundary and the remaining requirements for a public beta.

The codebase retains Bitcoin Core copyright notices and upstream attribution.

The current public identifier inventory and pre-launch registry warnings are in
[doc/connectcoin-branding.md](doc/connectcoin-branding.md).
The experimental consensus format and its compatibility boundaries are in
[doc/typed-outputs.md](doc/typed-outputs.md).
The P2C payload and proof profile are specified in
[doc/pay-to-connect.md](doc/pay-to-connect.md).
Creating bounties from the graphical wallet is described in
[doc/p2c-wallet.md](doc/p2c-wallet.md).

Quick links: [Installation](#installation) · [Getting started](#getting-started) ·
[Wallet data and backups](#wallet-data-and-backups) · [Updating](#updating) ·
[Testing](#testing) · [Translations](#translations).

Installation
------------

Run the command for your operating system **from the folder where you want to create `connectcoin`**. For example, running it in `Documents` creates `Documents/connectcoin`.

**What these commands do**

- Install the required packages and clone the `main` branch.
- Build the standard Release configuration with the Qt wallet, daemon, CLI, wallet tools, tests, benchmarks, and ZeroMQ support.
- Keep the source and build output inside `connectcoin`. System dependencies are installed through your package manager.
- Compile tests without running them. They do **not** launch the wallet or start mining.

**Before you start**

- Use a writable parent folder. Cloning stops if a nonempty `connectcoin` folder already exists; nothing is deleted.
- `--parallel 2` limits the final build to two concurrent jobs to reduce RAM usage. Increase it if your machine has enough memory.
- On Unix-like systems, `sudo` or `doas` is unnecessary when you are already logged in as `root`. It normally still works if installed; if it is missing, remove that prefix from the commands.
- Prefer cloning, compiling, and running the wallet as your normal user, using `sudo` or `doas` only to install system packages. Files created by `root` belong to `root`, which can cause permission problems when you later switch users.
- These instructions target the versions and configurations listed below, not every historical OS release or hardware architecture.

See the [dependency requirements](doc/dependencies.md) for minimum compiler and
library versions. These are source builds, not standalone installer packages:
keep their runtime dependencies installed. The standard build does not enable
specialized fuzzing or experimental kernel/chainstate targets. Additional Python
packages are needed for some [functional tests](#automated-testing).

### Ubuntu 24.04 / 26.04 and Debian 12 / 13

Ubuntu requires the `universe` repository to be enabled.

```bash
sudo apt-get update && sudo apt-get install -y git ca-certificates build-essential cmake ninja-build pkgconf python3 libboost-dev libsqlite3-dev capnproto libcapnp-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools qt6-tools-dev-tools qt6-wayland libgl-dev libqrencode-dev && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### Fedora

```bash
sudo dnf install -y git ca-certificates gcc-c++ make cmake ninja-build pkgconf python3 boost-devel sqlite-devel capnproto capnproto-devel zeromq-devel qt6-qtbase-devel qt6-qttools-devel qt6-qtwayland qrencode-devel && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### Arch Linux

**This command also upgrades system packages** using `-Syu` to avoid a partial upgrade.

```bash
sudo pacman -Syu --needed base-devel git ca-certificates cmake ninja pkgconf python boost sqlite capnproto zeromq qt6-base qt6-tools qt6-wayland qrencode && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### Alpine Linux 3.23

Enable the `main` and `community` repositories for your installed release. This command assumes `doas` is configured; remove it when running as root.

```sh
doas apk add --no-cache git ca-certificates build-base linux-headers cmake ninja pkgconf python3 boost-dev sqlite-dev capnproto capnproto-dev zeromq-dev qt6-qtbase-dev qt6-qtbase-x11 qt6-qttools-dev qt6-qtwayland libqrencode-dev && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### macOS — Apple Silicon, macOS 15 or later

Run the macOS commands as a normal user, not `root`; Homebrew refuses to run as root.

Installs Homebrew if missing. Its official installer attempts to install Apple's Command Line Tools and may request your password or display a confirmation window. ConnectCoin requires Command Line Tools **16.2 or later**.

```bash
(test -x /opt/homebrew/bin/brew || /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)") && eval "$(/opt/homebrew/bin/brew shellenv)" && brew install git cmake ninja boost capnp qt@6 qrencode zeromq pkgconf python && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

#### Intel Mac with Homebrew already configured

Homebrew currently provides reduced support for Intel Macs; package availability is not guaranteed to match Apple Silicon. The same Command Line Tools requirement applies.

```bash
brew install git cmake ninja boost capnp qt@6 qrencode zeromq pkgconf python && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### Windows x64 — Native Build

**First-time toolchain setup is a separate step:** installing Visual Studio may require restarting Windows or opening a fresh developer terminal. The project requires **Visual Studio 2026 version 18.3 or later**, with the C++ desktop workload, and **CMake 4.2 or later** for its Visual Studio generator.

Install missing tools from **Command Prompt (CMD)** using the applicable commands
below. Run them separately: an already-installed package can make WinGet return
a [nonzero exit status](https://github.com/microsoft/winget-cli/blob/master/doc/windows/package-manager/winget/returnCodes.md),
which would stop a chain joined with `&&`. If Visual Studio
is already installed without the C++ desktop workload, add that workload through
Visual Studio Installer before building.

```cmd
winget install --id Microsoft.VisualStudio.Community -e --override "--wait --quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.Git --includeRecommended"
winget install --id Python.Python.3.13 -e
```

Then open **Developer Command Prompt for VS 2026**, navigate to your chosen parent folder, and run this single build command. It downloads vcpkg and installs Qt, Boost, SQLite, and the other project libraries automatically.

```cmd
git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && git clone https://github.com/microsoft/vcpkg.git .vcpkg && call .vcpkg\bootstrap-vcpkg.bat -disableMetrics && set "VCPKG_MAX_CONCURRENCY=2" && cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=.vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --config Release --parallel 2
```

Use a short path without spaces to reduce Qt/vcpkg path issues. **Do not paste these CMD commands into Windows PowerShell 5.1**, which does not support `&&`.

### FreeBSD

Use `sh` or `bash`, with Clang **17 or later** and `sudo` configured.

```sh
sudo pkg install -y git ca_root_nss cmake boost-libs sqlite3 capnproto qt6-base qt6-tools libqrencode libzmq4 pkgconf python3 && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### NetBSD

Requires configured `pkgin` and `sudo`, with package repositories matching your installed OS release and architecture.

```sh
sudo pkgin update && sudo pkgin -y install git cmake gcc12 boost sqlite3 capnproto pkgconf qt6-qtbase qt6-qttools qrencode zeromq python313 && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/pkg/gcc12/bin/gcc -DCMAKE_CXX_COMPILER=/usr/pkg/gcc12/bin/g++ '-DCMAKE_PREFIX_PATH=/usr/pkg;/usr/pkg/qt6' -DPython3_EXECUTABLE=/usr/pkg/bin/python3.13 -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

### OpenBSD 7.9 — amd64

Requires the development and X11 installation sets, with `doas` configured.

```sh
doas pkg_add git cmake boost sqlite3 capnproto qt6-qtbase qt6-qttools libqrencode zeromq python%3.13 && git clone --branch main --depth 1 https://github.com/connectcoincrypto/connectcoin.git connectcoin && cd connectcoin && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPython3_EXECUTABLE=/usr/local/bin/python3.13 -DBUILD_GUI=ON -DENABLE_WALLET=ON -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DWITH_ZMQ=ON && cmake --build build --parallel 2
```

**BSD validation note:** these commands follow the project's build guides and requirements. CI does not validate every native BSD GUI configuration listed here; consult the platform-specific `doc/build-*bsd.md` guides for additional setup details.

### Launching the wallet

After the build completes, open a terminal in the `connectcoin` folder you created
and run the command for your operating system.

**Linux, macOS, and BSD:**

```sh
./build/bin/connectcoin-qt
```

**Windows (CMD or PowerShell):**

```text
.\build\bin\Release\connectcoin-qt.exe
```

Depending on your operating system and desktop environment, you can also launch
the wallet by double-clicking the executable in your file manager. On a VPS,
compiling over SSH does not install a desktop environment or configure graphical
remote access; opening the Qt wallet requires a graphical session.

This opens the ConnectCoin Qt wallet with an integrated full node, a CPU RandomX
miner (only when explicitly activated), Automatic Claims (only when explicitly
activated), and support for both pay-to-connect (P2C) and standard pay-to-public-key
transactions.

Getting started
---------------

### First launch

1. The beta defaults to **ConnectCoin testnet4**, not Bitcoin's testnet. Let the
   node connect to peers and synchronize before relying on the displayed balance.
2. A wallet is **not created automatically**. Choose **File > Create Wallet**
   or the **Create a new wallet** button. Use **File > Open Wallet** to open an
   existing wallet, or **File > Restore Wallet** for a backup.
3. **Automatic Claims** is the default wallet page, but HTTPS claiming remains
   disabled until you start it. Choose **P2C** to fund a bounty or **Send** for a
   standard public-key payment. See the [P2C wallet guide](doc/p2c-wallet.md).
4. **Mining** enables the optional solo CPU miner on testnet4 or regtest. Both
   mining and Automatic Claims are opt-in and start disabled after a restart.
   See [CPU mining](doc/cpu-mining.md) for controls and resource requirements.

RandomX FAST validation can use roughly 2 GiB per dataset **even with mining
disabled**. On memory-constrained machines, `-randomxfast=0` selects the slower,
lower-memory LIGHT mode. Mining threads and HTTPS connections also share your
computer and network with the node; increasing them can reduce responsiveness.

### Headless node on a VPS

The same build includes `connectcoind`, which does not require a desktop. On
Unix-like systems, start it from the source checkout with:

```sh
./build/bin/connectcoind -daemonwait
```

Inspect synchronization status with:

```sh
./build/bin/connectcoin-cli getblockchaininfo
```

To request a normal shutdown:

```sh
./build/bin/connectcoin-cli stop
```

Wait for the process to exit before restarting or rebuilding. On Windows, run
`.\build\bin\Release\connectcoind.exe` in one terminal without `-daemonwait`,
and use `.\build\bin\Release\connectcoin-cli.exe` from another terminal.
Do not run Qt and the daemon simultaneously against the same data directory.
The daemon enables RPC by default; to use the CLI with the Qt wallet instead,
start Qt with `-server`. Use matching network and custom `-datadir` options for
the node and CLI.

Testnet4 uses TCP **48179** for P2P and **48178** for RPC. Keep RPC private; it
does not need to be exposed for ordinary wallet use. See the
[testnet networking guide](doc/testnet-beta.md#testnet4-bootstrap-dns) for peer
discovery and public-node setup.

Wallet data and backups
----------------------

The source checkout and build directory are **not your wallet data directory**.
Unless you choose a custom location, beta data is stored here:

| Platform | Default testnet4 data directory |
| --- | --- |
| Linux / BSD | `$HOME/.connectcoin/testnet4/` |
| macOS | `$HOME/Library/Application Support/ConnectCoin/testnet4/` |
| Windows | `%LOCALAPPDATA%\ConnectCoin\testnet4\` |

Named wallets normally live in `wallets/<wallet-name>/wallet.dat` within that
network directory. Use **File > Backup Wallet** to create a consistent backup
and keep it outside the source checkout, preferably on a separate offline device.
Do not copy an active wallet database directly, publish wallet files or private
keys, or delete a wallet to resolve a build, lock, or chain-reset error.

For diagnostics, inspect `debug.log` in the network data directory; review logs
for sensitive information before sharing them. See [wallet management](doc/managing-wallets.md)
and the [data-directory reference](doc/files.md) for backups and custom paths.

Updating
--------

For an existing installation, do not clone over the checkout again. Back up your
wallets and shut down the wallet/node normally, then open a terminal in the
existing `connectcoin` folder. Check `git status --short` first and preserve any
local changes before continuing. An untracked `.vcpkg/` directory created by the
Windows instructions is expected; keep it. From `main` with no local changes to
tracked files, update and rebuild with the same build environment used for
installation:

```sh
git pull --ff-only && cmake -S . -B build && cmake --build build --config Release --parallel 2
```

On Windows, run that command in **Developer Command Prompt for VS 2026**.
Existing CMake options are retained; new dependency requirements may need the
corresponding package installation commands to be rerun. If Git reports local
changes or divergent history, resolve them instead of deleting or resetting
your work. These commands do not erase wallet data.

Read the [testnet compatibility notes](doc/testnet-beta.md) before adopting an
announced network reset. Rebuilding or reindexing does not convert old-chain
balances to a new chain, and a reset is not a reason to delete wallet backups.

License
-------

ConnectCoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/license/MIT.

Development Process
-------------------

The development branch should be built and tested after every consensus or
networking change. See [Installation](#installation) and the platform-specific
`doc/build-*.md` guides for build instructions.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md)
and useful hints for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

Testing and independent code review are essential for consensus, networking,
wallet, and P2C changes. Contributions should include reproducible test steps
and regression coverage for the behavior they change.

### Automated Testing

The installation commands build tests but do not run them. From the repository
root, run the registered tests with:

```sh
ctest --test-dir build --output-on-failure --parallel 1
```

For the Windows Release build, select its configuration explicitly:

```cmd
ctest --test-dir build --build-config Release --output-on-failure --parallel 1
```

The [functional tests](test/README.md) use Python. Some require additional
Python modules such as `pyzmq` or, for IPC builds, `pycapnp`; these are separate
from the native build dependencies above. Install the
[test prerequisites](test/README.md#dependencies-and-prerequisites) before
running the suite. Missing optional modules can cause tests to be skipped.

**Linux, macOS, and BSD:**

```sh
python3 -X utf8 build/test/functional/test_runner.py --jobs=1
```

**Windows:**

```cmd
py -3 -X utf8 build\test\functional\test_runner.py --jobs=1
```

These examples limit test concurrency to conserve memory; tests using real
RandomX can take time. See the [unit-test guide](src/test/README.md) for focused
tests and the [functional-test guide](test/README.md) for selecting individual
tests. Fuzzing requires a separate [fuzz build](doc/fuzzing.md).

[CI workflows](.github/workflows/ci.yml) exercise selected Linux, Windows, and
macOS configurations alongside cross-builds and other checks. Their coverage
does not imply that every OS configuration is tested. Applicable CI checks
should pass before changes are merged.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

ConnectCoin maintains the bundled Qt translation catalogs, including its P2C,
mining, and notification interfaces. Every bundled catalog is checked for full
coverage of the currently extracted messages. Runtime fallback to a base-language
catalog or English still exists, but does not count as a completed translation.

New translations are AI-assisted and are not a claim of native-speaker review.
See the [translation workflow](doc/translation_process.md) and the
[per-locale coverage report](doc/translation-coverage.md).
